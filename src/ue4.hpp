#pragma once

class UObjectBase;
class UClass;
class UStruct;
class UObject;

#include <cstdint>
#include <string>
#include <string_view>

#include "steelsdk/UClass.hpp"

struct FUObjectItem {
    UObjectBase* Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;
};

class FChunkedFixedUObjectArray {
public:
    static constexpr inline auto NumElementsPerChunk = 64 * 1024;

    __forceinline FUObjectItem const* GetObjectPtr(int32_t Index) const {
        const int32_t ChunkIndex = Index / NumElementsPerChunk;
        const int32_t WithinChunkIndex = Index % NumElementsPerChunk;

        auto Chunk = Objects[ChunkIndex];
        return Chunk + WithinChunkIndex;
    }

    __forceinline FUObjectItem* GetObjectPtr(int32_t Index) {
        const int32_t ChunkIndex = Index / NumElementsPerChunk;
        const int32_t WithinChunkIndex = Index % NumElementsPerChunk;

        auto Chunk = Objects[ChunkIndex];
        return Chunk + WithinChunkIndex;
    }

    __forceinline FUObjectItem const& operator[](int32_t Index) const {
        auto const* ItemPtr = GetObjectPtr(Index);
        return *ItemPtr;
    }

    __forceinline FUObjectItem& operator[](int32_t Index) {
        auto ItemPtr = GetObjectPtr(Index);
        return *ItemPtr;
    }

    __forceinline int32_t Num() const {
        return NumElements;
    }

    FUObjectItem** Objects;
    FUObjectItem* PreAllocatedObjects;
    int32_t MaxElements;
    int32_t NumElements;
    int32_t MaxChunks;
    int32_t NumChunks;
};

class FUObjectArray {
public:
    FUObjectItem* IndexToObject(int32_t Index);

    int32_t ObjFirstGCIndex;
    int32_t ObjLastNonGCIndex;
    int32_t MaxObjectsNotConsideredByGC;
    bool OpenForDisregardForGC;
    FChunkedFixedUObjectArray ObjObjects;

    // dont care didnt ask about the rest here.
};

template<typename T>
class TArray
{
public:
    __forceinline T* GetData() const {
        return Data;
    }

    __forceinline int32_t Num() const {
        return ArrayNum;
    }

    __forceinline T& operator[](int32_t Index) const {
        return Data[Index];
    }

    __forceinline T& operator[](int32_t Index) {
        return Data[Index];
    }

    // todo: implement add, remove, etc.

    T* Data;
    int32_t ArrayNum;
    int32_t ArrayMax;
};


class FString {
public:
    auto& GetCharArray() {
        return Data;
    }

    auto& GetCharArray() const {
        return Data;
    }

	TArray<wchar_t> Data;
};

class FName
{
public:
    FString ToString() const;

    int32_t ComparisonIndex;
    int32_t Number;
};

class UObjectBase {
public:
    virtual ~UObjectBase() {};

    bool IsA(UObject* SomeBase) const {
        const UStruct* SomeBaseClass = (UStruct*)SomeBase;
        const UStruct* ThisClass = ClassPrivate;

        __assume(ThisClass);
        __assume(SomeBaseClass);

        do {
            if (ThisClass == SomeBaseClass) {
                return true;
            }

            ThisClass = ThisClass->SuperStruct;
        } while(ThisClass != nullptr);

        return false;
    }

    bool IsA(UObjectBase* SomeBase) const {
        return IsA((UObject*)SomeBase);
    }

    uint32_t ObjectFlags;
    int32_t InternalIndex;
    UClass* ClassPrivate;
    FName NamePrivate;
    UObject* OuterPrivate;
};

class FUObjectArray* get_GUObjectArray();
FUObjectItem* find_uobject(std::string_view obj_path);
FUObjectItem* find_uobject(size_t obj_path_hash);
std::string get_full_name(UObjectBase* obj);
std::string get_full_name(UObject* obj);
std::string narrow(const FString& fstr);
std::string narrow(const FName& fname);