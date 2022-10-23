#include <ue4.hpp>

#undef TEXT

#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>

#include <uevr/API.hpp>

using namespace uevr;

static constexpr auto GUOBJECTARRAY_PAT = "48 8D 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 ? 48 83 ? 10 00 74 ? 8B ? 0C";
static constexpr auto FNAME_TOSTRING_PAT = "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC ? 8B 01";
constexpr int UOBJECT_PROCESSEVENT_INDEX = 68;

FString FName::ToString() const {
    FString out;

    static void (*toString)(const FName*, FString&){};

    if (toString == nullptr) {
        API::get()->log_info("Finding FName::ToString...");

        toString = (decltype(toString))utility::scan(utility::get_executable(), FNAME_TOSTRING_PAT).value_or(0);

        if (toString == nullptr) {
            API::get()->log_error("Failed to find FName::ToString!");
        } else {
            API::get()->log_info("Found FName::ToString @ 0x%llx", (uint64_t)toString);
        }
    }

    toString(this, out);
    return out;
}

FUObjectArray* get_GUObjectArray() {
    static FUObjectArray* obj_array{};

    if (obj_array == nullptr) {
        API::get()->log_info("Finding GUObjectArray...");

        auto lea = utility::scan(utility::get_executable(), GUOBJECTARRAY_PAT);

        if (!lea) {
            API::get()->log_error("Failed to find GUObjectArray!");
            return nullptr;
        }

        obj_array = (FUObjectArray*)utility::calculate_absolute(*lea + 3);

        API::get()->log_info("Found GUObjectArray!");
    }

    return obj_array;
}

FUObjectItem* find_uobject(std::string_view obj_path) {
    const auto gobj = get_GUObjectArray();

    if (gobj == nullptr) {
        return nullptr;
    }

    for (auto i = 0; i < gobj->ObjObjects.Num(); ++i) {
        const auto obj = gobj->ObjObjects[i].Object;

        if (obj != nullptr) {
            const auto name = get_full_name(obj);

            if (name == obj_path) {
                return &gobj->ObjObjects[i];
            }
        }
    }

    return nullptr;
}

FUObjectItem* find_uobject(size_t obj_path_hash) {
    const auto gobj = get_GUObjectArray();

    if (gobj == nullptr) {
        return nullptr;
    }

    for (auto i = 0; i < gobj->ObjObjects.Num(); ++i) {
        const auto obj = gobj->ObjObjects[i].Object;

        if (obj != nullptr) {
            const auto name = get_full_name(obj);

            if (utility::hash(name) == obj_path_hash) {
                return &gobj->ObjObjects[i];
            }
        }
    }

    return nullptr;
}

FUObjectItem* FUObjectArray::IndexToObject(int32_t Index) {
    if (Index < ObjObjects.Num()) {
        return const_cast<FUObjectItem *>(&ObjObjects[Index]);
    }

    return nullptr;
}

std::string narrow(const FString& fstr) {
    auto& char_array = fstr.GetCharArray();
    return utility::narrow(char_array.Num() && char_array.GetData() != nullptr ? char_array.GetData() : L"");
}

std::string narrow(const FName& fname) {
    return narrow(fname.ToString());
}

std::string get_full_name(UObjectBase* obj) {
    if (obj == nullptr) {
        return "null";
    }

    auto c = obj->ClassPrivate;

    if (c == nullptr) {
        return "null";
    }

    auto obj_name = narrow(obj->NamePrivate);

    for (auto outer = (UObjectBase*)obj->OuterPrivate; outer != nullptr; outer = (UObjectBase*)outer->OuterPrivate) {
        obj_name = narrow(outer->NamePrivate) + '.' + obj_name;
    }

    return narrow(((UObjectBase*)c)->NamePrivate) + ' ' + obj_name;
}

std::string get_full_name(UObject* obj) {
    return get_full_name((UObjectBase*)obj);
}
