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

#include <sdk/Math.hpp>

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

class ExamplePlugin : public uevr::Plugin {
public:
    ExamplePlugin() = default;

    void on_dllmain() override {}

    void on_initialize() override {
        ImGui::CreateContext();

        API::get()->log_error("%s %s", "Hello", "error");
        API::get()->log_warn("%s %s", "Hello", "warning");
        API::get()->log_info("%s %s", "Hello", "info");

        auto gobj = get_GUObjectArray();

        /*for (auto i = 0; i < 10000; ++i) {
            const auto obj = gobj->ObjObjects[i].Object;

            if (obj) {
                const auto name = get_full_name(obj);

                API::get()->log_info("%s", name.c_str());
            }
        }*/
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
        PLUGIN_LOG_ONCE("Example Device Reset");

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
        auto pawn = get_pawn(m_engine);

        m_player_exists = pawn != nullptr;

        if (pawn == nullptr) {
            return;
        }

        PLUGIN_LOG_ONCE("Pawn: 0x%p", (uintptr_t)pawn);
        PLUGIN_LOG_ONCE("Pawn class: %s", get_full_name(pawn->ClassPrivate).c_str());

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

                const auto quat_asdf = glm::quat{Matrix4x4f {
                    0, 0, -1, 0,
                    1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 0, 1
                }};

                const auto offset1 = quat_asdf * (glm::normalize(view_quat_inverse_flat) * (pos * world_to_meters));
                const auto offset2 = quat_asdf * (glm::normalize(view_quat_inverse) * (eye_offset * world_to_meters));
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

                    auto hands = pawn->CurrentlyEquippedWeapon;

                    if (hands != nullptr) {
                        m_hands_exists = true;
                        // first attempt at motion controls
                        Vector3f right_hand_position{};
                        glm::quat right_hand_rotation{};
                        vr->get_pose(vr->get_right_controller_index(), (UEVR_Vector3f*)&right_hand_position, (UEVR_Quaternionf*)&right_hand_rotation);

                        Vector3f left_hand_position{};
                        glm::quat left_hand_rotation{};
                        vr->get_pose(vr->get_left_controller_index(), (UEVR_Vector3f*)&left_hand_position, (UEVR_Quaternionf*)&left_hand_rotation);

                        right_hand_position = glm::vec3{rotation_offset * (right_hand_position - standing_origin)};
                        left_hand_position = glm::vec3{rotation_offset * (left_hand_position - standing_origin)};

                        right_hand_position = quat_asdf * (glm::normalize(view_quat_inverse_flat) * (right_hand_position * world_to_meters));
                        left_hand_position = quat_asdf * (glm::normalize(view_quat_inverse_flat) * (left_hand_position * world_to_meters));

                        right_hand_position = *(Vector3f*)position - right_hand_position;
                        left_hand_position = *(Vector3f*)position - left_hand_position;

                        right_hand_rotation = rotation_offset * right_hand_rotation;
                        right_hand_rotation = (glm::normalize(view_quat_inverse_flat) * right_hand_rotation);

                        auto right_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(right_hand_rotation));

                        // swap yaw and roll
                        const auto tmp_yaw = right_hand_euler.y;
                        const auto tmp_roll = right_hand_euler.z;
                        //right_hand_euler.y = -tmp_roll;
                        //right_hand_euler.z = tmp_yaw;

                        FHitResult r1{}, r2{};
                        hands->K2_SetActorLocation(*(FVector*)&right_hand_position, false, r1, false);
                        hands->K2_SetActorRotation(*(FRotator*)&right_hand_euler, false);
                        //hands->K2_SetWorldLocation(*(FVector*)&right_hand_position, false, r1, false);
                    } else {
                        m_hands_exists = false;
                    }
                }

                *(Vector3f*)position -= offset2;
            }
        }
    }

private:
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

            ImGui::End();
        }
    }

private:
    HWND m_wnd{};
    bool m_initialized{false};
    bool m_player_exists{false};
    bool m_hands_exists{false};

    UGameEngine* m_engine{};

    glm::quat m_last_vr_rotation{};
    Vector3f m_last_pos_svo{};
    float m_prev_yaw_svo{};

    FRotator m_facegun_rotator{};
    FRotator m_right_hand_rotation_offset{-90.0f, 0.0f, 0.0f};
    FRotator m_left_hand_rotation_offset{-90.0f, 0.0f, 0.0f};
};

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<ExamplePlugin> g_plugin{new ExamplePlugin()};
