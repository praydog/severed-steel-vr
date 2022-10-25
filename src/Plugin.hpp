#pragma once

#include <windows.h>

#include <memory>
#include <uevr/Plugin.hpp>
#include <safetyhook/InlineHook.hpp>

#include "steelsdk/EImpactType.hpp"
#include "steelsdk/FRotator.hpp"
#include "steelsdk/FVector.hpp"
#include "steelsdk/FHitResult.hpp"
#include "Math.hpp"

#define PLUGIN_LOG_ONCE(...) { \
    static bool _logged_ = false; \
    if (!_logged_) { \
        _logged_ = true; \
        API::get()->log_info(__VA_ARGS__); \
    } }

// Global accessor for our plugin.
class SteelPlugin;
extern std::unique_ptr<SteelPlugin> g_plugin;

class SteelPlugin : public uevr::Plugin {
public:
    SteelPlugin() = default;
    virtual ~SteelPlugin();

    // Main plugin callbacks
    void on_dllmain() override;
    void on_initialize() override;
    void on_present() override;
    void on_device_reset() override;
    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override;
    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override;

    // Game/Engine callbacks
    void on_pre_engine_tick(UEVR_UGameEngineHandle engine, float delta) override;
    void on_post_engine_tick(UEVR_UGameEngineHandle engine, float delta) override;
    void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override;
    void on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override;
    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                                     UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override;
    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                                      UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override;

private:
    bool initialize_imgui();
    void internal_frame();

private: // Getters
    class ::APlayerCharacter_BP_Manny_C* get_pawn(class ::UGameEngine* engine);
    class ::UWorld* get_world(class ::UGameEngine*);

private:
    void hook_resolve_impact();

    FRotator facegun(class ::APlayerCharacter_BP_Manny_C* pawn, FRotator& real_rot);

    void update_weapon_traces(class ::APlayerCharacter_BP_Manny_C* pawn);
    bool on_resolve_impact_internal(class ::AImpactManager* mgr, FHitResult& HitResult, EImpactType Impact, bool FiredByPlayer, class ::AActor* Shooter, FVector& TraceOrigin, float PenetrationModifier, bool bAlreadyKilledNPC);

    static bool on_resolve_impact(AImpactManager* mgr, FHitResult& HitResult, EImpactType Impact, bool FiredByPlayer, class ::AActor* Shooter, FVector& TraceOrigin, float PenetrationModifier, bool bAlreadyKilledNPC) {
        return g_plugin->on_resolve_impact_internal(mgr, HitResult, Impact, FiredByPlayer, Shooter, TraceOrigin, PenetrationModifier, bAlreadyKilledNPC);
    }

    std::unique_ptr<safetyhook::InlineHook> m_resolve_impact_hook{};

    HWND m_wnd{};
    bool m_initialized{false};
    bool m_player_exists{false};
    bool m_hands_exists{false};
    uint32_t m_num_localplayers{0};

    class ::UGameEngine* m_engine{};
    class ::UWorld* m_world{};
    class ::APawn* m_last_pawn{nullptr};
    class ::AActor* m_last_weapon{nullptr};
    class ::AActor* m_last_fired_actor{nullptr};

    glm::quat m_last_vr_rotation{};
    Vector3f m_last_pos_svo{};
    float m_prev_yaw_svo{};

    FRotator m_facegun_rotator{};
    FRotator m_right_hand_rotation_offset{-68.0f, -8.0f, 24.0f};
    FRotator m_left_hand_rotation_offset{-90.0f, 0.0f, 0.0f};

    FHitResult m_right_hand_weapon_hr{};

    uint32_t m_resolve_impact_depth{0};
};