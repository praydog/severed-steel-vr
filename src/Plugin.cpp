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
#include "steelsdk/UTYVCAnimInstance.hpp"

#include <safetyhook/Factory.hpp>
#include <safetyhook/MidHook.hpp>
#include "Math.hpp"
#include <utility/Module.hpp>
#include <utility/Scan.hpp>

#include "Plugin.hpp"

using namespace uevr;

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<SteelPlugin> g_plugin{new SteelPlugin()};

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

// Called on unload
SteelPlugin::~SteelPlugin() {
    if (m_resolve_impact_hook) {
        m_resolve_impact_hook.reset();
    }
}

void SteelPlugin::on_dllmain() {

}

void SteelPlugin::on_initialize() {
    ImGui::CreateContext();

    API::get()->log_error("%s %s", "Hello", "error");
    API::get()->log_warn("%s %s", "Hello", "warning");
    API::get()->log_info("%s %s", "Hello", "info");

    hook_resolve_impact();
}

void SteelPlugin::hook_resolve_impact() {
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

void SteelPlugin::on_present() {
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

void SteelPlugin::on_device_reset() {
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

bool SteelPlugin::on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

    return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
}

void SteelPlugin::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
    PLUGIN_LOG_ONCE("XInput Get State");

    const auto param = API::get()->param();
    const auto vr = param->vr;

    if (vr->get_lowest_xinput_index() != user_index || !vr->is_using_controllers()) {
        return;
    }

    // dont do anything if we are drawing the ui in the framework.
    if (param->functions->is_drawing_ui()) {
        return;
    }

    // reset because the injector sets some generic inputs that are not suitable for this game
    // but dont remove the start button because we need it to open the menu
    state->Gamepad.wButtons &= XINPUT_GAMEPAD_START;
    state->Gamepad.bRightTrigger = 0;
    state->Gamepad.bLeftTrigger = 0;

    const auto right_joystick_source = vr->get_right_joystick_source();
    const auto left_joystick_source = vr->get_left_joystick_source();

    const auto a_button_action_left = vr->get_action_handle("/actions/default/in/AButtonLeft");
    const auto a_button_action_right = vr->get_action_handle("/actions/default/in/AButtonRight");
    const auto is_right_a_button_down = vr->is_action_active_any_joystick(a_button_action_right);
    const auto is_left_a_button_down = vr->is_action_active_any_joystick(a_button_action_left);

    if (is_right_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    }

    const auto b_button_action_left = vr->get_action_handle("/actions/default/in/BButtonLeft");
    const auto b_button_action_right = vr->get_action_handle("/actions/default/in/BButtonRight");
    const auto is_right_b_button_down = vr->is_action_active_any_joystick(b_button_action_right);
    const auto is_left_b_button_down = vr->is_action_active_any_joystick(b_button_action_left);

    if (is_right_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    }

    if (is_left_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    }

    if (is_left_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    const auto joystick_click_action = vr->get_action_handle("/actions/default/in/JoystickClick");
    const auto is_left_joystick_click_down = vr->is_action_active(joystick_click_action, left_joystick_source);
    const auto is_right_joystick_click_down = vr->is_action_active(joystick_click_action, right_joystick_source);

    if (is_left_joystick_click_down) {
        // we don't map this to L3 because it's just jump
        // so we bind it to the dive button instead as it's more convenient
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    if (is_right_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    const auto trigger_action = vr->get_action_handle("/actions/default/in/Trigger");
    const auto is_left_trigger_down = vr->is_action_active(trigger_action, left_joystick_source);
    const auto is_right_trigger_down = vr->is_action_active(trigger_action, right_joystick_source);

    if (is_left_trigger_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // not the same as LT
    }

    if (is_right_trigger_down) {
        state->Gamepad.bRightTrigger = 255;
    }

    const auto grip_action = vr->get_action_handle("/actions/default/in/Grip");
    const auto is_right_grip_down = vr->is_action_active(grip_action, right_joystick_source);

    if (is_right_grip_down) {
        state->Gamepad.bLeftTrigger = 255; // LT
    }

    Vector2f left_joystick_axis{};
    vr->get_joystick_axis(left_joystick_source, (UEVR_Vector2f*)&left_joystick_axis);

    Vector2f right_joystick_axis{};
    vr->get_joystick_axis(right_joystick_source, (UEVR_Vector2f*)&right_joystick_axis);

    state->Gamepad.sThumbLX = (int16_t)(left_joystick_axis.x * 32767.0f);
    state->Gamepad.sThumbLY = (int16_t)(left_joystick_axis.y * 32767.0f);

    state->Gamepad.sThumbRX = (int16_t)(right_joystick_axis.x * 32767.0f);
    state->Gamepad.sThumbRY = (int16_t)(right_joystick_axis.y * 32767.0f);

    if (right_joystick_axis.y <= -0.9f) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_B; // Slide
    }

    if (right_joystick_axis.y >= 0.9f) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y; // Dive
    }

    *retval = ERROR_SUCCESS;
}

void SteelPlugin::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    PLUGIN_LOG_ONCE("XInput Set State");

    auto vr = API::get()->param()->vr;

    if (!vr->is_using_controllers()) {
        return;
    }

    const auto right_joystick_source = vr->get_right_joystick_source();
    const auto left_joystick_source = vr->get_left_joystick_source();
    const auto left_amplitude = ((float)vibration->wLeftMotorSpeed / 65535.0f) * 5.0f;
    const auto right_amplitude = ((float)vibration->wRightMotorSpeed / 65535.0f) * 5.0f;
    const auto total_amp = (left_amplitude + right_amplitude);
    vr->trigger_haptic_vibration(0.0f, 0.1f, 1.0f, left_amplitude, left_joystick_source);
    vr->trigger_haptic_vibration(0.0f, 0.1f, 1.0f, right_amplitude, right_joystick_source);
}

APlayerCharacter_BP_Manny_C* SteelPlugin::get_pawn(UGameEngine* engine) {
    auto instance = engine->GameInstance;

    if (instance != nullptr) {
        m_num_localplayers = instance->LocalPlayers.Num();
    }

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

UWorld* SteelPlugin::get_world(UGameEngine* engine) {
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

FRotator SteelPlugin::facegun(APlayerCharacter_BP_Manny_C* pawn, FRotator& real_rot) {
    auto rot = pawn->GetFirstPersonCamera()->K2_GetComponentRotation();
    auto component_q = utility::math::flatten(glm::yawPitchRoll(
                    glm::radians(-rot.Yaw),
                    glm::radians(rot.Pitch),
                    glm::radians(-rot.Roll)));
    
    const auto vqi_norm = glm::normalize(component_q);

    glm::quat hmd_q{};
    Vector3f pos{};

    const auto vr = API::get()->param()->vr;
    vr->get_pose(vr->get_hmd_index(), (UEVR_Vector3f*)&pos, (UEVR_Quaternionf*)&hmd_q);

    glm::quat rot_offset{};
    vr->get_rotation_offset((UEVR_Quaternionf*)&rot_offset);

    //hmd_q = rot_offset * hmd_q;
   /* const auto hmd_flatq = utility::math::flatten(hmd_q);

    const auto delta_towards_q = glm::normalize(hmd_flatq * glm::inverse(m_last_vr_rotation));
    m_last_vr_rotation = hmd_flatq;

    const auto hmd_no_forward = glm::inverse(hmd_flatq) * hmd_q;*/

    const auto current_hmd_rotation = glm::normalize(rot_offset * hmd_q);
    const auto new_rotation = glm::normalize(vqi_norm * current_hmd_rotation);
    const auto new_rotation_flat = utility::math::flatten(new_rotation);
    const auto angles = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

    //component_q = glm::normalize(delta_towards_q * component_q * hmd_no_forward);
    //const auto component_forward_q = utility::math::to_quat(component_q * Vector3f{ 0.0f, 0.0f, 1.0f });

    //const auto angles = glm::degrees(utility::math::euler_angles_from_steamvr(component_forward_q));
    rot.Yaw = angles.y;
    rot.Pitch = angles.x;
    rot.Roll = angles.z;
    
    const auto angles_real = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation_flat));
    real_rot.Yaw = angles_real.y;
    real_rot.Pitch = angles_real.x;
    real_rot.Roll = angles_real.z;

    FHitResult r{};
    pawn->GetFirstPersonCamera()->K2_SetWorldRotation(rot, false, r, false);

    hmd_q = utility::math::flatten(glm::inverse(hmd_q));
    //vr->set_rotation_offset((UEVR_Quaternionf*)&hmd_q);
    vr->recenter_view();
    //pawn->K2_SetActorRotation(rot, false);

    return rot;
}

void SteelPlugin::on_pre_engine_tick(UEVR_UGameEngineHandle engine_handle, float delta) {
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

void SteelPlugin::on_post_engine_tick(UEVR_UGameEngineHandle engine, float delta) {
    PLUGIN_LOG_ONCE("Post Engine Tick: %f", delta);
}

void SteelPlugin::on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) {
    PLUGIN_LOG_ONCE("Pre Slate Draw Window");
}

void SteelPlugin::on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) {
    PLUGIN_LOG_ONCE("Post Slate Draw Window");
}

void SteelPlugin::on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                                      UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double)
{
    PLUGIN_LOG_ONCE("Pre Calculate Stereo View Offset");

    if (this->m_player_exists) {
        m_prev_yaw_svo = rotation->yaw;
        m_last_pos_svo = *(Vector3f*)position;
    }

    //rotation->pitch = 0.0f;
    //rotation->roll = 0.0f;

    if (this->m_player_exists) {
        auto pawn = get_pawn(m_engine);

        if (pawn != nullptr) {
            auto weapon_ptr = (AWeaponBase**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)pawn, L"CurrentlyEquippedWeapon"));
            auto weapon = weapon_ptr != nullptr ? *weapon_ptr : nullptr;
            m_last_weapon = weapon;

            *(Vector3f*)position = m_last_pos_svo;

            if ((view_index + 1) % 2 == 0) {
                //rotation->pitch = 0.0f;
                //rotation->roll = 0.0f;
                //rotation->yaw = m_prev_yaw_svo;
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
            //*(Vector3f*)position -= offset1;

            if ((view_index + 1) % 2 == 0) {
                const auto actor_position = pawn->K2_GetActorLocation();
                const auto delta_move = offset1 * -1.0f;
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

                const auto final_position = *(Vector3f*)position - offset1;

                ///////////////////////////////////
                // first attempt at motion controls
                ///////////////////////////////////
                if (vr->is_using_controllers()) {
                    Vector3f right_hand_position{};
                    glm::quat right_hand_rotation{};
                    vr->get_grip_pose(vr->get_right_controller_index(), (UEVR_Vector3f*)&right_hand_position, (UEVR_Quaternionf*)&right_hand_rotation);

                    Vector3f left_hand_position{};
                    glm::quat left_hand_rotation{};
                    vr->get_grip_pose(vr->get_left_controller_index(), (UEVR_Vector3f*)&left_hand_position, (UEVR_Quaternionf*)&left_hand_rotation);

                    right_hand_position = glm::vec3{rotation_offset * (right_hand_position - hmd_origin)};
                    left_hand_position = glm::vec3{rotation_offset * (left_hand_position - hmd_origin)};

                    right_hand_position = quat_to_ue4 * (glm::normalize(view_quat_inverse_flat) * (right_hand_position * world_to_meters));
                    left_hand_position = quat_to_ue4 * (glm::normalize(view_quat_inverse_flat) * (left_hand_position * world_to_meters));

                    right_hand_position = final_position - right_hand_position;
                    left_hand_position = final_position - left_hand_position;

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

                    auto arm_cannon_ptr = (AArmCannon**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)pawn, L"ArmCannon"));
                    auto arm_cannon = arm_cannon_ptr != nullptr ? *arm_cannon_ptr : nullptr;

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

                    auto hands_ptr = (USkeletalMeshComponent**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)pawn, L"Hands"));
                    auto hands = hands_ptr != nullptr ? *hands_ptr : nullptr;

                    // Hide the player model
                    if (hands != nullptr) {
                        hands->SetHiddenInGame(true, false);
                    }
                }
            }

            // Eye offset. Apply it at the very end so the eye itself doesn't get used as the actor's position, but rather the center of the head.
            //*(Vector3f*)position -= offset2;
        }
    }
}

void SteelPlugin::on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                                       UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double)
{
    PLUGIN_LOG_ONCE("Post Calculate Stereo View Offset");
}

bool SteelPlugin::on_resolve_impact_internal(AImpactManager* mgr, FHitResult& HitResult, EImpactType Impact, bool FiredByPlayer, AActor* Shooter, FVector& TraceOrigin, float PenetrationModifier, bool bAlreadyKilledNPC) {
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

    // After
    auto weapon_ptr = (AWeaponBase**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)pawn, L"CurrentlyEquippedWeapon"));
    auto weapon = weapon_ptr != nullptr ? *weapon_ptr : nullptr;

    if (weapon == nullptr) {
        return call_orig();
    }

    auto muzzle = (UPointLightComponent**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)weapon, L"MuzzleFlashPointLight"));

    if (muzzle != nullptr && *muzzle != nullptr) {
        if (update_weapon_traces(pawn)) {
            HitResult = m_right_hand_weapon_hr;
        }

        //ctx.rdx = (uintptr_t)&m_right_hand_weapon_hr;
    }

    return call_orig();
}

bool SteelPlugin::update_weapon_traces(APlayerCharacter_BP_Manny_C* pawn) try {
    auto weapon_ptr = (AWeaponBase**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)pawn, L"CurrentlyEquippedWeapon"));
    auto weapon = weapon_ptr != nullptr ? *weapon_ptr : nullptr;

    auto muzzle_ptr = weapon != nullptr ? (UPointLightComponent**)(API::get()->sdk()->uobject->get_property_data((UEVR_UObjectHandle)weapon, L"MuzzleFlashPointLight")) : nullptr;
    auto muzzle = muzzle_ptr != nullptr ? *muzzle_ptr : nullptr;

    if (weapon == nullptr || m_world == nullptr || muzzle == nullptr) {
        return false;
    }
    
    const auto vr = API::get()->param()->vr;

    if (!vr->is_using_controllers()) {
        return false;
    }

    const auto start = ((USceneComponent*)muzzle)->K2_GetComponentLocation();
    const auto& start_glm = *(glm::vec3*)&start;

    const auto rot = ((USceneComponent*)muzzle)->K2_GetComponentRotation();
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

    return true;
} catch (...) {
    PLUGIN_LOG_ONCE_ERROR("update_weapon_traces failed");
    return false;
}

bool SteelPlugin::initialize_imgui() {
    if (m_initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::GetIO().IniFilename = "severed_steel_ui.ini";
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

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

void SteelPlugin::internal_frame() {
    if (!API::get()->param()->functions->is_drawing_ui()) {
        return;
    }

    if (ImGui::Begin("Severed Steel")) {
        bool exists = m_player_exists;
        ImGui::Checkbox("Player exists", &exists);
        ImGui::Checkbox("Hands exists", &m_hands_exists);

        ImGui::DragFloat3("Right Hand Rotation Offset", &m_right_hand_rotation_offset.Pitch);
        ImGui::DragFloat3("Left Hand Rotation Offset", &m_left_hand_rotation_offset.Pitch);

        ImGui::Text("Last Pawn: 0x%p", (uintptr_t)m_last_pawn);
        ImGui::Text("Last Weapon: 0x%p", (uintptr_t)m_last_weapon);
        ImGui::Text("Last Fired Actor: 0x%p", (uintptr_t)m_last_fired_actor);
        ImGui::Text("Num localplayers: %i", m_num_localplayers);

        ImGui::End();
    }
}

