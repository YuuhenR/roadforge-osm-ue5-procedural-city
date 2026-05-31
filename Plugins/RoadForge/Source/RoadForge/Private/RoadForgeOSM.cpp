// Copyright RoadForge Contributors. Licensed under the MIT License.

#include "RoadForgeOSM.h"

#include "XmlFile.h"
#include "XmlNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadForgeOSM, Log, All);

namespace RoadForgeOSM
{
	bool IsXmlFile(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path).ToLower();
		if (Ext == TEXT("osm") || Ext == TEXT("xml")) { return true; }
		if (Ext == TEXT("json")) { return false; }
		// Unknown extension: sniff the first non-whitespace byte of the file.
		FString Head;
		FFileHelper::LoadFileToString(Head, *Path, FFileHelper::EHashOptions::None, 256);
		Head.TrimStartInline();
		return Head.StartsWith(TEXT("<"));
	}

	bool ParseXml(const FString& XmlText, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		FXmlFile Xml(XmlText, EConstructMethod::ConstructFromBuffer);
		if (!Xml.IsValid())
		{
			OutError = FString::Printf(TEXT("XML parse error: %s"), *Xml.GetLastError());
			return false;
		}
		const FXmlNode* OsmRoot = Xml.GetRootNode();
		if (!OsmRoot)
		{
			OutError = TEXT("XML has no root <osm> node.");
			return false;
		}

		const TArray<FXmlNode*>& Children = OsmRoot->GetChildrenNodes();

		// Pass 1: id -> (lat, lon). OSM lists every node once, before the ways that reference it.
		TMap<int64, TPair<double, double>> NodeLatLon;
		NodeLatLon.Reserve(Children.Num());
		for (const FXmlNode* Child : Children)
		{
			if (!Child || Child->GetTag() != TEXT("node")) { continue; }
			const int64 Id = FCString::Atoi64(*Child->GetAttribute(TEXT("id")));
			const double Lat = FCString::Atod(*Child->GetAttribute(TEXT("lat")));
			const double Lon = FCString::Atod(*Child->GetAttribute(TEXT("lon")));
			NodeLatLon.Add(Id, TPair<double, double>(Lat, Lon));
		}

		// Pass 2: ways -> Overpass-style way objects (only the highway / building ways the generator uses).
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const FXmlNode* Way : Children)
		{
			if (!Way || Way->GetTag() != TEXT("way")) { continue; }

			const TArray<FXmlNode*>& WayKids = Way->GetChildrenNodes();

			// Collect tags first so we can skip irrelevant ways (saves a lot of geometry on big files).
			TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
			bool bRelevant = false;
			for (const FXmlNode* Kid : WayKids)
			{
				if (!Kid || Kid->GetTag() != TEXT("tag")) { continue; }
				const FString K = Kid->GetAttribute(TEXT("k"));
				const FString V = Kid->GetAttribute(TEXT("v"));
				if (K.IsEmpty()) { continue; }
				Tags->SetStringField(K, V);
				if (K == TEXT("highway") || K == TEXT("building")) { bRelevant = true; }
			}
			if (!bRelevant) { continue; }

			// Resolve node refs -> ids + geometry.
			TArray<TSharedPtr<FJsonValue>> NodesArr, GeomArr;
			for (const FXmlNode* Kid : WayKids)
			{
				if (!Kid || Kid->GetTag() != TEXT("nd")) { continue; }
				const int64 Ref = FCString::Atoi64(*Kid->GetAttribute(TEXT("ref")));
				const TPair<double, double>* P = NodeLatLon.Find(Ref);
				if (!P) { continue; } // node outside the export bounds
				NodesArr.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Ref)));
				TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
				G->SetNumberField(TEXT("lat"), P->Key);
				G->SetNumberField(TEXT("lon"), P->Value);
				GeomArr.Add(MakeShared<FJsonValueObject>(G));
			}
			if (GeomArr.Num() < 2) { continue; }

			TSharedRef<FJsonObject> WayObj = MakeShared<FJsonObject>();
			WayObj->SetStringField(TEXT("type"), TEXT("way"));
			WayObj->SetNumberField(TEXT("id"), FCString::Atod(*Way->GetAttribute(TEXT("id"))));
			WayObj->SetObjectField(TEXT("tags"), Tags);
			WayObj->SetArrayField(TEXT("nodes"), NodesArr);
			WayObj->SetArrayField(TEXT("geometry"), GeomArr);
			Elements.Add(MakeShared<FJsonValueObject>(WayObj));
		}

		OutRoot = MakeShared<FJsonObject>();
		OutRoot->SetArrayField(TEXT("elements"), Elements);

		UE_LOG(LogRoadForgeOSM, Log, TEXT("Parsed OSM XML: %d nodes, %d relevant ways."),
			NodeLatLon.Num(), Elements.Num());
		return Elements.Num() > 0;
	}

	bool LoadOSM(const FString& Path, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read '%s'."), *Path);
			return false;
		}

		if (IsXmlFile(Path))
		{
			return ParseXml(Text, OutRoot, OutError);
		}

		// Overpass JSON already has the {"elements":[...]} shape; just deserialize it.
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse JSON (%d bytes)."), Text.Len());
			return false;
		}
		return true;
	}

	bool LoadOSMText(const FString& Path, FString& OutJsonText, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read '%s'."), *Path);
			return false;
		}

		// A .json export is already Overpass-style text — hand it straight to BuildFromJson.
		if (!IsXmlFile(Path))
		{
			OutJsonText = MoveTemp(Text);
			return true;
		}

		// A raw .osm XML export is converted to the same shape, then serialised to compact JSON text.
		TSharedPtr<FJsonObject> Root;
		if (!ParseXml(Text, Root, OutError))
		{
			return false;
		}
		OutJsonText.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJsonText);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return true;
	}
}
