// Install DebugView to view the OutputDebugString calls
#include <sstream>
#include <mutex>
#include <memory>

#include <Windows.h>

// only really necessary if you want to render to the screen
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

#include "ue4.hpp"

#include "steelsdk/UGameEngine.hpp"
#include "steelsdk/UGameInstance.hpp"
#include "steelsdk/ULocalPlayer.hpp"
#include "steelsdk/ATYVCPC_BP_C.hpp"
#include "steelsdk/APlayerCharacter_BP_Manny_C.hpp"
#include "steelsdk/UCameraComponent.hpp"
#include "steelsdk/USkeletalMeshComponent.hpp"
#include "steelsdk/AWeaponBase.hpp"
#include "steelsdk/AArmCannon.hpp"
#include "steelsdk/UKismetSystemLibrary.hpp"
#include "steelsdk/UKismetStringLibrary.hpp"
#include "steelsdk/UGameViewportClient.hpp"
#include "steelsdk/UWorld.hpp"
#include "steelsdk/UPawnMovementComponent.hpp"
#include "steelsdk/UInputComponent.hpp"

#include <safetyhook/Factory.hpp>
#include <safetyhook/MidHook.hpp>
#include <sdk/Math.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>

using namespace uevr;

template <typename T> 
T* DCast(UObject* In) {
    if (In == nullptr) {
        return nullptr;
    }

    //API::get()->log_info("Comparing %s and %s", get_full_name(In).c_str(), get_full_name(T::StaticClass()).c_str());

    if (((UObjectBase*)In)->IsA(T::StaticClass())) {
        return (T*)In;
    }

    return nullptr;
}


#define PLUGIN_LOG_ONCE(...) { \
    static bool _logged_ = false; \
    if (!_logged_) { \
        _logged_ = true; \
        API::get()->log_info(__VA_ARGS__); \
    } }

class SteelPlugin;
extern std::unique_ptr<SteelPlugin> g_plugin;

FString fstring_from_chars(std::wstring_view chars) {
    FString str;
    str.Data.Data = (wchar_t*)chars.data();
    str.Data.ArrayNum = chars.size();
    str.Data.ArrayMax = chars.size();
    return str;
}

FName fname_from_chars(std::wstring_view chars) {
    const auto str = fstring_from_chars(chars);
    return UKismetStringLibrary::Conv_StringToName(str);
}

class SteelPlugin : public uevr::Plugin {
public:
    SteelPlugin() = default;

    // Called on unload
    virtual ~SteelPlugin() {
        if (m_resolve_impact_hook) {
            m_resolve_impact_hook.reset();
        }
    }

    void on_dllmain() override {}

    void on_initialize() override {
        ImGui::CreateContext();

        API::get()->log_error("%s %s", "Hello", "error");
        API::get()->log_warn("%s %s", "Hello", "warning");
        API::get()->log_info("%s %s", "Hello", "info");

        hook_resolve_impact();
    }

    void hook_resolve_impact() {
        API::get()->log_info("Hooking AImpactManager::ResolveImpact");

        auto factory = safetyhook::Factory::init();
        auto builder = factory->acquire();

        auto item = find_uobject(15725550492628957501);

        if (item != nullptr) {
            auto obj = (UFunction*)(item->Object);

            if (obj != nullptr) {
                const auto func_wrapper = *(void**)((uintptr_t)obj + sizeof(UStruct) + 0x28);

                if (func_wrapper != nullptr) {
                    // Scan for the real function using disassembly
                    // the moment we find a ret instruction, use the last call as the real function
                    auto ip = (uintptr_t)func_wrapper;

                    bool found_ret = false;
                    uintptr_t last_function_called = 0;

                    for (auto i = 0; i < 200; ++i) {
                        const auto decoded = utility::decode_one((uint8_t*)ip);

                        if (!decoded) {
                            break;
                        }

                        if (*(uint8_t*)ip == 0xE8) {
                            last_function_called = utility::calculate_absolute(ip + 1);
                        }

                        if (*(uint8_t*)ip == 0xC3 || *(uint8_t*)ip == 0xC2 || *(uint8_t*)ip == 0xCC) {
                            found_ret = true;
                            break;
                        }

                        ip += decoded->Length;
                    }

                    if (found_ret && last_function_called != 0) {
                        API::get()->log_info("Found real function at %p", last_function_called);

                        const auto game = utility::get_executable();
                        m_resolve_impact_hook = builder.create_inline((void*)last_function_called, &on_resolve_impact);
                    }
                }
            }
        }
    }

    void on_present() override {
        if (!m_initialized) {
            if (!initialize_imgui()) {
                API::get()->log_info("Failed to initialize imgui");
                return;
            } else {
                API::get()->log_info("Initialized imgui");
            }
        }

        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            internal_frame();

            ImGui::EndFrame();
            ImGui::Render();

            g_d3d11.render_imgui();
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            auto command_queue = (ID3D12CommandQueue*)renderer_data->command_queue;

            if (command_queue == nullptr ){
                return;
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            internal_frame();

            ImGui::EndFrame();
            ImGui::Render();

            g_d3d12.render_imgui();
        }
    }

    void on_device_reset() override {
        PLUGIN_LOG_ONCE("Device Reset");

        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        m_initialized = false;
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override { 
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override {
        PLUGIN_LOG_ONCE("XInput Get State");

        auto vr = API::get()->param()->vr;

        const auto right_joystick_source = vr->get_right_joystick_source();
        const auto left_joystick_source = vr->get_left_joystick_source();

        const auto a_button_action = vr->get_action_handle("/actions/default/in/AButton");
        const auto is_right_a_button_down = vr->is_action_active(a_button_action, right_joystick_source);
        const auto is_left_a_button_down = vr->is_action_active(a_button_action, left_joystick_source);

        if (is_right_a_button_down) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
        } else {
        }

        const auto b_button_action = vr->get_action_handle("/actions/default/in/BButton");
        const auto is_right_b_button_down = vr->is_action_active(b_button_action, right_joystick_source);
        const auto is_left_b_button_down = vr->is_action_active(b_button_action, left_joystick_source);

        if (is_right_b_button_down) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
        } else {
        }

        if (is_left_a_button_down) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
        } else {
        }

        if (is_left_b_button_down) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
        } else {
        }

        const auto joystick_click_action = vr->get_action_handle("/actions/default/in/JoystickClick");
        const auto is_left_joystick_click_down = vr->is_action_active(joystick_click_action, left_joystick_source);

        if (is_left_joystick_click_down) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
        }


        Vector2f left_joystick_axis{};
        vr->get_joystick_axis(left_joystick_source, (UEVR_Vector2f*)&left_joystick_axis);

        Vector2f right_joystick_axis{};
        vr->get_joystick_axis(right_joystick_source, (UEVR_Vector2f*)&right_joystick_axis);

        state->Gamepad.sThumbLX = (int16_t)(left_joystick_axis.x * 32767.0f);
        state->Gamepad.sThumbLY = (int16_t)(left_joystick_axis.y * 32767.0f);

        state->Gamepad.sThumbRX = (int16_t)(right_joystick_axis.x * 32767.0f);
        state->Gamepad.sThumbRY = (int16_t)(right_joystick_axis.y * 32767.0f);

        *retval = ERROR_SUCCESS;
    }

    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override {
        PLUGIN_LOG_ONCE("XInput Set State");
    }

    APlayerCharacter_BP_Manny_C* get_pawn(UGameEngine* engine) {
        auto instance = engine->GameInstance;

        if (instance == nullptr || instance->LocalPlayers.Num() == 0) {
            return nullptr;
        }

        auto player = instance->LocalPlayers[0];

        if (player == nullptr) {
            return nullptr;
        }

        PLUGIN_LOG_ONCE("Player: 0x%p", (uintptr_t)player);

        auto controller = DCast<ATYVCPC_BP_C>(player->PlayerController);

        if (controller == nullptr) {
            return nullptr;
        }

        PLUGIN_LOG_ONCE("Controller: 0x%p", (uintptr_t)controller);
        PLUGIN_LOG_ONCE("Controller class: %s", get_full_name(controller->ClassPrivate).c_str());

        return DCast<APlayerCharacter_BP_Manny_C>(controller->AcknowledgedPawn);
    }

    UWorld* get_world(UGameEngine* engine) {
        auto instance = engine->GameInstance;

        if (instance == nullptr || instance->LocalPlayers.Num() == 0) {
            return nullptr;
        }

        auto player = instance->LocalPlayers[0];

        if (player == nullptr) {
            return nullptr;
        }

        auto vp_client = player->ViewportClient;

        if (vp_client == nullptr) {
            return nullptr;
        }

        return vp_client->World;
    }

    FRotator facegun(APlayerCharacter_BP_Manny_C* pawn, FRotator& real_rot) {
        auto rot = pawn->GetFirstPersonCamera()->K2_GetComponentRotation();
        auto component_q = utility::math::flatten(glm::yawPitchRoll(
                        glm::radians(-rot.Yaw),
                        glm::radians(rot.Pitch),
                        glm::radians(-rot.Roll)));

        glm::quat hmd_q{};
        Vector3f pos{};

        const auto vr = API::get()->param()->vr;
        vr->get_pose(vr->get_hmd_index(), (UEVR_Vector3f*)&pos, (UEVR_Quaternionf*)&hmd_q);

        glm::quat rot_offset{};
        vr->get_rotation_offset((UEVR_Quaternionf*)&rot_offset);

        //hmd_q = rot_offset * hmd_q;
        const auto hmd_flatq = utility::math::flatten(hmd_q);

        const auto delta_towards_q = glm::normalize(hmd_flatq * glm::inverse(m_last_vr_rotation));
        m_last_vr_rotation = hmd_flatq;

        const auto hmd_no_forward = glm::inverse(hmd_flatq) * hmd_q;
        component_q = glm::normalize(delta_towards_q * component_q * hmd_no_forward);

        const auto component_forward_q = utility::math::to_quat(component_q * Vector3f{ 0.0f, 0.0f, 1.0f });

        const auto angles = glm::degrees(utility::math::euler_angles_from_steamvr(component_forward_q));
        rot.Yaw = angles.y;
        rot.Pitch = angles.x;
        rot.Roll = angles.z;
        
        const auto angles_real = glm::degrees(utility::math::euler_angles_from_steamvr(component_q));
        real_rot.Yaw = angles_real.y;
        real_rot.Pitch = angles_real.x;
        real_rot.Roll = angles_real.z;

        FHitResult r{};
        pawn->GetFirstPersonCamera()->K2_SetWorldRotation(rot, false, r, false);

        hmd_q = utility::math::flatten(glm::inverse(hmd_q));
        vr->set_rotation_offset((UEVR_Quaternionf*)&hmd_q);
        //pawn->K2_SetActorRotation(rot, false);

        return rot;
    }

    void on_pre_engine_tick(UEVR_UGameEngineHandle engine_handle, float delta) override {
        PLUGIN_LOG_ONCE("Pre Engine Tick: %f", delta);

        m_engine = (UGameEngine*)engine_handle;
        m_world = get_world(m_engine);
        m_last_pawn = get_pawn(m_engine);

        m_player_exists = m_last_pawn != nullptr;

        

        if (m_last_pawn == nullptr) {
            return;
        }

        PLUGIN_LOG_ONCE("Pawn: 0x%p", (uintptr_t)m_last_pawn);
        PLUGIN_LOG_ONCE("Pawn class: %s", get_full_name(m_last_pawn->ClassPrivate).c_str());

        //auto& rot = controller->TargetViewRotation;
    }

    void on_post_engine_tick(UEVR_UGameEngineHandle engine, float delta) override {
        PLUGIN_LOG_ONCE("Post Engine Tick: %f", delta);
    }

    void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        PLUGIN_LOG_ONCE("Pre Slate Draw Window");
    }

    void on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        PLUGIN_LOG_ONCE("Post Slate Draw Window");
    }

    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                             UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override
    {
        PLUGIN_LOG_ONCE("Pre Calculate Stereo View Offset");

        if (this->m_player_exists) {
            m_prev_yaw_svo = rotation->yaw;
            m_last_pos_svo = *(Vector3f*)position;
        }

        rotation->pitch = 0.0f;
        rotation->roll = 0.0f;
    }

    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                              UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double)
    {
        PLUGIN_LOG_ONCE("Post Calculate Stereo View Offset");

        if (this->m_player_exists) {
            auto pawn = get_pawn(m_engine);

            if (pawn != nullptr) {
                m_last_weapon = pawn->CurrentlyEquippedWeapon;

                *(Vector3f*)position = m_last_pos_svo;

                if ((view_index + 1) % 2 == 0) {
                    rotation->pitch = 0.0f;
                    rotation->roll = 0.0f;
                    rotation->yaw = m_prev_yaw_svo;
                    facegun(pawn, *(FRotator*)rotation);

                    m_facegun_rotator = *(FRotator*)rotation;
                } else {
                    rotation->pitch = m_facegun_rotator.Pitch;
                    rotation->roll = m_facegun_rotator.Roll;
                    rotation->yaw = m_facegun_rotator.Yaw;
                }

                // we need to fix the position after facegun now because the
                // rotation is different than what the injector set
                const auto view_mat_inverse =
                    glm::yawPitchRoll(
                        glm::radians(-rotation->yaw),
                        glm::radians(rotation->pitch),
                        glm::radians(-rotation->roll));

                const auto view_quat_inverse = glm::quat {
                    view_mat_inverse
                };

                const auto view_quat_inverse_flat = utility::math::flatten(view_quat_inverse);
                
                const auto vr = API::get()->param()->vr;

                glm::quat rotation_offset{};
                vr->get_rotation_offset((UEVR_Quaternionf*)&rotation_offset);

                Vector3f standing_origin{};
                vr->get_standing_origin((UEVR_Vector3f*)&standing_origin);

                Vector3f hmd_origin{};
                glm::quat hmd_rotation{};
                vr->get_pose(vr->get_hmd_index(), (UEVR_Vector3f*)&hmd_origin, (UEVR_Quaternionf*)&hmd_rotation);

                const auto pos = glm::vec3{rotation_offset * (hmd_origin - standing_origin)};

                Vector3f eye_offset{};
                vr->get_eye_offset((view_index + 1) % 2, (UEVR_Vector3f*)&eye_offset);

                const auto quat_to_ue4 = glm::quat{Matrix4x4f {
                    0, 0, -1, 0,
                    1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 0, 1
                }};

                const auto offset1 = quat_to_ue4 * (glm::normalize(view_quat_inverse_flat) * (pos * world_to_meters));
                const auto offset2 = quat_to_ue4 * (glm::normalize(view_quat_inverse) * (eye_offset * world_to_meters));
                *(Vector3f*)position -= offset1;

                if ((view_index + 1) % 2 == 0) {
                    const auto actor_position = pawn->K2_GetActorLocation();
                    const auto delta_move = *(Vector3f*)position - m_last_pos_svo;
                    const auto adjusted_position = Vector3f {
                        actor_position.X + delta_move.x,
                        actor_position.Y + delta_move.y,
                        actor_position.Z
                    };

                    FHitResult r{};
                    pawn->K2_SetActorLocation(*(FVector*)&adjusted_position, false, r, false);

                    standing_origin.x = hmd_origin.x;
                    standing_origin.z = hmd_origin.z;
                    vr->set_standing_origin((UEVR_Vector3f*)&standing_origin);

                    ///////////////////////////////////
                    // first attempt at motion controls
                    ///////////////////////////////////
                    auto weapon = pawn->CurrentlyEquippedWeapon;

                    Vector3f right_hand_position{};
                    glm::quat right_hand_rotation{};
                    vr->get_pose(vr->get_right_controller_index(), (UEVR_Vector3f*)&right_hand_position, (UEVR_Quaternionf*)&right_hand_rotation);

                    Vector3f left_hand_position{};
                    glm::quat left_hand_rotation{};
                    vr->get_pose(vr->get_left_controller_index(), (UEVR_Vector3f*)&left_hand_position, (UEVR_Quaternionf*)&left_hand_rotation);

                    right_hand_position = glm::vec3{rotation_offset * (right_hand_position - hmd_origin)};
                    left_hand_position = glm::vec3{rotation_offset * (left_hand_position - hmd_origin)};

                    right_hand_position = quat_to_ue4 * (glm::normalize(view_quat_inverse_flat) * (right_hand_position * world_to_meters));
                    left_hand_position = quat_to_ue4 * (glm::normalize(view_quat_inverse_flat) * (left_hand_position * world_to_meters));

                    right_hand_position = *(Vector3f*)position - right_hand_position;
                    left_hand_position = *(Vector3f*)position - left_hand_position;

                    right_hand_rotation = rotation_offset * right_hand_rotation;
                    right_hand_rotation = (glm::normalize(view_quat_inverse_flat) * right_hand_rotation);

                    left_hand_rotation = rotation_offset * left_hand_rotation;
                    left_hand_rotation = (glm::normalize(view_quat_inverse_flat) * left_hand_rotation);

                    const auto right_hand_offset_q = glm::quat{glm::yawPitchRoll(
                        glm::radians(m_right_hand_rotation_offset.Yaw),
                        glm::radians(m_right_hand_rotation_offset.Pitch),
                        glm::radians(m_right_hand_rotation_offset.Roll))
                    };

                    const auto left_hand_offset_q = glm::quat{glm::yawPitchRoll(
                        glm::radians(m_left_hand_rotation_offset.Yaw),
                        glm::radians(m_left_hand_rotation_offset.Pitch),
                        glm::radians(m_left_hand_rotation_offset.Roll))
                    };

                    right_hand_rotation = glm::normalize(right_hand_rotation * right_hand_offset_q);
                    auto right_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(right_hand_rotation));

                    left_hand_rotation = glm::normalize(left_hand_rotation * left_hand_offset_q);
                    auto left_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(left_hand_rotation));

                    if (weapon != nullptr) {
                        m_hands_exists = true;

                        FHitResult r1{};
                        weapon->K2_SetActorLocation(*(FVector*)&right_hand_position, false, r1, false);
                        weapon->K2_SetActorRotation(*(FRotator*)&right_hand_euler, false);
                        auto transform = weapon->GetTransform();
                        transform.Scale3D.X = 1.0f;
                        transform.Scale3D.Y = 1.0f;
                        transform.Scale3D.Z = 1.0f;

                        FHitResult r2{};
                        weapon->K2_SetActorTransform(transform, false, r1, false);
                    } else {
                        m_hands_exists = false;
                    }

                    auto arm_cannon = pawn->ArmCannon;

                    if (arm_cannon != nullptr) {
                        FHitResult r1{};
                        arm_cannon->K2_SetActorLocation(*(FVector*)&left_hand_position, false, r1, false);
                        arm_cannon->K2_SetActorRotation(*(FRotator*)&left_hand_euler, false);

                        auto transform = arm_cannon->GetTransform();
                        transform.Scale3D.X = 1.0f;
                        transform.Scale3D.Y = 1.0f;
                        transform.Scale3D.Z = 1.0f;

                        FHitResult r2{};
                        arm_cannon->K2_SetActorTransform(transform, false, r2, false);
                    }

                    
                    handle_input(pawn, *(Vector3f*)position, *(FRotator*)rotation);
                    //update_weapon_traces(pawn);

                    // Hide the player model
                    if (pawn->Hands != nullptr) {
                        pawn->Hands->SetHiddenInGame(true, false);
                    }
                }

                // Eye offset. Apply it at the very end so the eye itself doesn't get used as the actor's position, but rather the center of the head.
                *(Vector3f*)position -= offset2;
            }
        }
    }

private:
    uint32_t m_resolve_impact_depth{0};
    bool on_resolve_impact_internal(AImpactManager* mgr, FHitResult& HitResult, EImpactType Impact, bool FiredByPlayer, AActor* Shooter, FVector& TraceOrigin, float PenetrationModifier, bool bAlreadyKilledNPC) {
        PLUGIN_LOG_ONCE("on_resolve_impact_internal");
        m_last_fired_actor = Shooter;

        auto call_orig = [&]() -> bool {
            try {
                ++m_resolve_impact_depth;
                if (m_resolve_impact_depth > 10) {
                    return false;
                }
                const auto result = m_resolve_impact_hook->call<bool>(mgr, &HitResult, Impact, FiredByPlayer, Shooter, &TraceOrigin, PenetrationModifier, bAlreadyKilledNPC);
                --m_resolve_impact_depth;
                return result;
            } catch(...) {
                return false;
            }
        };

        if (m_resolve_impact_depth > 0) {
            return call_orig();
        }

        if (m_last_fired_actor == nullptr) {
            return call_orig();
        }

        if (m_last_fired_actor != m_last_pawn) {
            return call_orig();
        }

        const auto pawn = DCast<APlayerCharacter_BP_Manny_C>(m_last_pawn);

        if (pawn == nullptr) {
            return call_orig();
        }

        auto weapon = pawn->CurrentlyEquippedWeapon;

        if (weapon == nullptr) {
            return call_orig();
        }

        if (weapon->MuzzleFlashPointLight != nullptr) {
            update_weapon_traces(pawn);
            HitResult = m_right_hand_weapon_hr;
            //ctx.rdx = (uintptr_t)&m_right_hand_weapon_hr;
        }

        return call_orig();
    }

    static bool on_resolve_impact(AImpactManager* mgr, FHitResult& HitResult, EImpactType Impact, bool FiredByPlayer, AActor* Shooter, FVector& TraceOrigin, float PenetrationModifier, bool bAlreadyKilledNPC) {
        return g_plugin->on_resolve_impact_internal(mgr, HitResult, Impact, FiredByPlayer, Shooter, TraceOrigin, PenetrationModifier, bAlreadyKilledNPC);
    }

    void update_weapon_traces(APlayerCharacter_BP_Manny_C* pawn) try {
        auto weapon = pawn->CurrentlyEquippedWeapon;

        if (weapon == nullptr || m_world == nullptr || weapon->MuzzleFlashPointLight == nullptr) {
            return;
        }
        
        const auto start = ((USceneComponent*)weapon->MuzzleFlashPointLight)->K2_GetComponentLocation();
        const auto& start_glm = *(glm::vec3*)&start;

        const auto rot = ((USceneComponent*)weapon->MuzzleFlashPointLight)->K2_GetComponentRotation();
        const auto rot_glm = glm::quat{glm::yawPitchRoll(
            glm::radians(-rot.Yaw),
            glm::radians(rot.Pitch),
            glm::radians(-rot.Roll))
        };

        const auto quat_to_ue4 = glm::quat{Matrix4x4f {
            0, 0, -1, 0,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1
        }};

        const auto end_glm = start_glm + (quat_to_ue4 * rot_glm * glm::vec3{0, 0.0f, 8192.0f});
        
        TArray_Plugin<AActor*> ignore_actors{};
        ignore_actors.Add(pawn);

        FLinearColor color{1.0f, 1.0f, 1.0f, 1.0f};
        m_right_hand_weapon_hr = {};
        UKismetSystemLibrary::LineTraceSingle(m_world, start, *(FVector*)&end_glm, ETraceTypeQuery::TraceTypeQuery16, true, *(TArray<AActor*>*)&ignore_actors, EDrawDebugTrace::None, m_right_hand_weapon_hr, true, color, color, 0.0f);
    } catch (...) {
        PLUGIN_LOG_ONCE("update_weapon_traces failed");
    }

    void handle_input(APlayerCharacter_BP_Manny_C* pawn, Vector3f& position, FRotator& rotation) {
        auto vr = API::get()->param()->vr;

        const auto left_joystick_source = vr->get_left_joystick_source();
        const auto right_joystick_source = vr->get_right_joystick_source();

        Vector2f left_joystick_axis{};
        vr->get_joystick_axis(left_joystick_source, (UEVR_Vector2f*)&left_joystick_axis);

        Vector2f right_joystick_axis{};
        vr->get_joystick_axis(right_joystick_source, (UEVR_Vector2f*)&right_joystick_axis);

        /*if (left_joystick_axis.length() > 0.0f && m_was_forward_down) {
            pawn->ForwardPressed();
            m_was_forward_down = false;
        } else {
            pawn->ForwardReleased();
            m_was_forward_down = true;
        }*/

        const auto rot_flat_q = utility::math::flatten(glm::quat{glm::yawPitchRoll(
            glm::radians(-rotation.Yaw),
            glm::radians(rotation.Pitch),
            glm::radians(-rotation.Roll))
        });

        auto fwd = rot_flat_q * glm::vec3{1.0f, 0.0f, 0.0f};

        const auto quat_to_ue4 = glm::quat{Matrix4x4f {
            0, 0, -1, 0,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1
        }};


        // rotate left joystick axis by the player's rotation
        const auto corrected_left_joystick = quat_to_ue4 * rot_flat_q * Vector3f{-left_joystick_axis.x, 0.0f, left_joystick_axis.y};

        //pawn->AddMovementInput(*(FVector*)&corrected_left_joystick, 1.0f, false);
        /*pawn->GetMovementComponent()->AddInputVector(*(FVector*)&corrected_left_joystick, false);

        FHitResult r{};
        pawn->GetFirstPersonCamera()->K2_AddLocalRotation(FRotator{0.0f, right_joystick_axis.x, 0.0f}, false, r, false);*/

        const auto a_button_action = vr->get_action_handle("/actions/default/in/AButton");
        const auto is_right_a_button_down = vr->is_action_active(a_button_action, right_joystick_source);
        const auto is_left_a_button_down = vr->is_action_active(a_button_action, left_joystick_source);

        if (is_right_a_button_down) {
            //pawn->bPressedJump = true;
        } else {

        }

        /*if (is_left_a_button_down && !m_was_left_a_button_down) {
            pawn->KickSlidePressedController();
            m_was_left_a_button_down = true;
        } else if (!is_left_a_button_down && m_was_left_a_button_down) {
            pawn->KickSlideReleasedController();
            m_was_left_a_button_down = false;
        }*/

        const auto b_button_action = vr->get_action_handle("/actions/default/in/BButton");
        const auto is_right_b_button_down = vr->is_action_active(b_button_action, right_joystick_source);
        const auto is_left_b_button_down = vr->is_action_active(b_button_action, left_joystick_source);

        /*if (is_left_b_button_down && !m_was_left_b_button_down) {
            pawn->DiveController();
            m_was_left_b_button_down = true;
        } else if (!is_left_b_button_down && m_was_left_b_button_down) {
            pawn->DiveReleased();
            m_was_left_b_button_down = false;
        }*/

        /*if (is_right_b_button_down) {
            pawn->PickupCallPressedController();
        }*/

        const auto grip_action = vr->get_action_handle("/actions/default/in/Grip");
        const auto is_right_grip_down = vr->is_action_active(grip_action, right_joystick_source);

        if (is_right_grip_down) {
            pawn->EnterSlowMo();
            m_was_right_grip_down = true;
        } else if (!is_right_grip_down && m_was_right_grip_down) {
            pawn->LeaveSlowMo();
            m_was_right_grip_down = false;
        }
        
        const auto joystick_click_action = vr->get_action_handle("/actions/default/in/JoystickClick");
        const auto is_right_joystick_click_down = vr->is_action_active(joystick_click_action, right_joystick_source);

        if (is_right_joystick_click_down && !m_was_right_joystick_click_down) {
            pawn->KickPressed();
            m_was_right_joystick_click_down = true;
        } else if (!is_right_joystick_click_down && m_was_right_joystick_click_down) {
            pawn->KickReleased();
            m_was_right_joystick_click_down = false;
        }

        const auto trigger_action = vr->get_action_handle("/actions/default/in/Trigger");
        const auto is_left_trigger_down = vr->is_action_active(trigger_action, left_joystick_source);
        const auto is_right_trigger_down = vr->is_action_active(trigger_action, right_joystick_source);

        if (is_left_trigger_down && !m_was_left_trigger_down) {
            pawn->ShootCannonGamepadPressed();
            m_was_left_trigger_down = true;
        } else {
            pawn->ShootCannonGamepadReleased();
            m_was_left_trigger_down = false;
        }

        if (is_right_trigger_down && !m_was_right_trigger_down) {
            pawn->TriggerDownController();
            m_was_right_trigger_down = true;
        } else if (!is_right_trigger_down && m_was_right_trigger_down) {
            pawn->TriggerUpController();
            m_was_right_trigger_down = false;
        }
    }

    bool m_was_left_a_button_down{false};
    bool m_was_left_b_button_down{false};
    bool m_was_right_grip_down{false};
    bool m_was_right_joystick_click_down{false};
    bool m_was_left_trigger_down{false};
    bool m_was_right_trigger_down{false};
    
    bool m_was_forward_down{false};

    bool initialize_imgui() {
        if (m_initialized) {
            return true;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGui::GetIO().IniFilename = "example_dll_ui.ini";

        const auto renderer_data = API::get()->param()->renderer;

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
        swapchain->GetDesc(&swap_desc);

        m_wnd = swap_desc.OutputWindow;

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        }

        m_initialized = true;
        return true;
    }

    void internal_frame() {
        if (ImGui::Begin("Severed Steel")) {
            bool exists = m_player_exists;
            ImGui::Checkbox("Player exists", &exists);
            ImGui::Checkbox("Hands exists", &m_hands_exists);

            ImGui::DragFloat3("Right Hand Rotation Offset", &m_right_hand_rotation_offset.Pitch);
            ImGui::DragFloat3("Left Hand Rotation Offset", &m_left_hand_rotation_offset.Pitch);

            ImGui::Text("Last Pawn: 0x%p", (uintptr_t)m_last_pawn);
            ImGui::Text("Last Weapon: 0x%p", (uintptr_t)m_last_weapon);
            ImGui::Text("Last Fired Actor: 0x%p", (uintptr_t)m_last_fired_actor);

            ImGui::End();
        }
    }

private:
    std::unique_ptr<safetyhook::InlineHook> m_resolve_impact_hook{};

    HWND m_wnd{};
    bool m_initialized{false};
    bool m_player_exists{false};
    bool m_hands_exists{false};

    UGameEngine* m_engine{};
    UWorld* m_world{};
    APawn* m_last_pawn{nullptr};
    AActor* m_last_weapon{nullptr};
    AActor* m_last_fired_actor{nullptr};

    glm::quat m_last_vr_rotation{};
    Vector3f m_last_pos_svo{};
    float m_prev_yaw_svo{};

    FRotator m_facegun_rotator{};
    FRotator m_right_hand_rotation_offset{-68.0f, -8.0f, 24.0f};
    FRotator m_left_hand_rotation_offset{-90.0f, 0.0f, 0.0f};

    FHitResult m_right_hand_weapon_hr{};
};

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<SteelPlugin> g_plugin{new SteelPlugin()};
