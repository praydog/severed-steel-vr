print("Initializing hello_world.lua")

UEVR_UObjectHook.activate()

local api = uevr.api

local game_engine_class = api:find_uobject("Class /Script/Engine.GameEngine")
local gameplay_hud_c = api:find_uobject("Class /Script/ThankYouVeryCool.GameplayHUD")
local actor_c = api:find_uobject("Class /Script/Engine.Actor")
local gameplay_statics_c = api:find_uobject("Class /Script/Engine.GameplayStatics")
local gameplay_statics = gameplay_statics_c:get_class_default_object()
local kismet_system_library_c = api:find_uobject("Class /Script/Engine.KismetSystemLibrary")
local kismet_system_library = kismet_system_library_c:get_class_default_object()
local kismet_math_library_c = api:find_uobject("Class /Script/Engine.KismetMathLibrary")
local kismet_math_library = kismet_math_library_c:get_class_default_object()
local kismet_string_library_c = api:find_uobject("Class /Script/Engine.KismetStringLibrary")
local kismet_string_library = kismet_string_library_c:get_class_default_object()
local ftransform_c = api:find_uobject("ScriptStruct /Script/CoreUObject.Transform")
local widget_component_c = api:find_uobject("Class /Script/UMG.WidgetComponent")
local static_mesh_component_c = api:find_uobject("Class /Script/Engine.StaticMeshComponent")
local sphere_static_mesh = api:find_uobject("StaticMesh /Engine/BasicShapes/Cube.Cube")
local sphere_component_c = api:find_uobject("Class /Script/Engine.SphereComponent")
local hitresult_c = api:find_uobject("ScriptStruct /Script/Engine.HitResult")
local zero_hit_result = StructObject.new(hitresult_c)
local zero_transform = StructObject.new(ftransform_c)
zero_transform.Scale3D = Vector3f.new(1.0, 1.0, 1.0)
zero_transform.Rotation.W = 1.0


local color_c = api:find_uobject("ScriptStruct /Script/CoreUObject.LinearColor")
local zero_color = StructObject.new(color_c)

local reusable_hit_result = StructObject.new(hitresult_c)
local temp_transform = StructObject.new(ftransform_c)

local temp_actor = nil

local lobber_c = api:find_uobject("BlueprintGeneratedClass /Game/Weapons/BP_M203_Round_Lobber.BP_M203_Round_Lobber_C")
local launch_fn = lobber_c:find_function("Launch")

if launch_fn ~= nil then
    print("Found Launch function")

    launch_fn:hook_ptr(nil, function(fn, obj, locals, result)
        print("Launched! " .. tostring(locals))

        -- get locals def
        local desc = locals:get_struct()
        print("Locals: " .. tostring(desc))

        -- Print all fields
        local first = desc:get_child_properties()

        while first ~= nil do
            print("Field: " .. first:get_fname():to_string() .. " = " .. tostring(locals[first:get_fname():to_string()]))
            first = first:get_next()
        end
    end)
else
    print("Failed to find Launch function")
end

--[[local lobber_top = gameplay_hud_c

while lobber_top ~= nil do
    local lobber_field = lobber_top:get_children()

    while lobber_field ~= nil do
        local field_c = lobber_field:get_class()
        local c_name = field_c:get_fname():to_string()
        if c_name == "Function" then
            local func = lobber_field:as_function()
            if func ~= nil and func:get_fname():to_string():find("K2_") == nil then
                print("Function: " .. func:get_fname():to_string())

                -- Hook test
                func:hook_ptr(nil, function(fn, obj, locals, result)
                    print("Hooked function: " .. fn:get_fname():to_string())
                end)
            end
        else
            print("Field: " .. lobber_field:get_fname():to_string())
        end

        lobber_field = lobber_field:get_next()
    end

    lobber_top = lobber_top:get_super()
end]]

local function spawn_actor(world_context, actor_class, location, collision_method, owner)
    temp_transform.Translation = location
    temp_transform.Rotation.W = 1.0
    temp_transform.Scale3D = Vector3f.new(1.0, 1.0, 1.0)

    local actor = gameplay_statics:BeginDeferredActorSpawnFromClass(world_context, actor_class, temp_transform, collision_method, owner)

    if actor == nil then
        print("Failed to spawn actor")
        return nil
    end

    gameplay_statics:FinishSpawningActor(actor, temp_transform)

    return actor
end

local function reset_temp_actor()
    if temp_actor ~= nil then
        temp_actor:K2_DestroyActor()
        temp_actor = nil
    end
end

local last_impact_pos = Vector3f.new(0.0, 0.0, 0.0)
local fire_fx_slot_fname = kismet_string_library:Conv_StringToName("Fire_FX_Slot")

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta_time)
    local game_engine = UEVR_UObjectHook.get_first_object_by_class(game_engine_class)

    local viewport = game_engine.GameViewport
    if viewport == nil then
        print("Viewport is nil")
        return
    end

    local world = viewport.World
    if world == nil then
        print("World is nil")
        return
    end

    local gameplay_hud = gameplay_hud_c:get_first_object_matching(false)
    if gameplay_hud == nil then return end

    local crosshair_widget = gameplay_hud.CrosshairWidget
    if crosshair_widget == nil then return end

    local game_instance = game_engine.GameInstance

    if game_instance == nil then
        print("GameInstance is nil")
        return
    end

    local local_players = game_instance.LocalPlayers
    local local_player = local_players[1];

    if local_player == nil then
        print("Local player is nil")
        return
    end

    local player_controller = local_player.PlayerController
    local pawn = player_controller.Pawn
    
    if temp_actor == nil and pawn ~= nil then
        local pos = pawn:K2_GetActorLocation()
        temp_actor = spawn_actor(world, actor_c, pos, 1, nil)

        if temp_actor == nil then
            print("Failed to spawn actor")
            return
        end

        print("Spawned actor")

        temp_transform.Translation = pos
        temp_transform.Rotation.W = 1.0
        temp_transform.Scale3D = Vector3f.new(1.0, 1.0, 1.0)
        local widget_component = temp_actor:AddComponentByClass(widget_component_c, false, temp_transform, false)

        if widget_component == nil then
            print("Failed to add widget component")
            return
        end

        print("Added widget component")

        -- Add crosshair widget to the widget component
        crosshair_widget:RemoveFromViewport()
        --crosshair_widget:AddToViewport(0)
        widget_component:SetWidget(crosshair_widget)
        widget_component:SetVisibility(true)
        widget_component:SetHiddenInGame(false)
        widget_component:SetCollisionEnabled(0)
        temp_actor:FinishAddComponent(widget_component, false, temp_transform)
        widget_component:SetWidget(crosshair_widget)

        -- Disable depth testing
        widget_component:SetRenderCustomDepth(true)
        widget_component:SetCustomDepthStencilValue(100)
        widget_component:SetCustomDepthStencilWriteMask(1)

        print("Widget space: " .. tostring(widget_component.Space))
        print("Widget draw size: X=" .. widget_component.DrawSize.X .. ", Y=" .. widget_component.DrawSize.Y)
        print("Widget visibility: " .. tostring(widget_component:IsVisible()))

        --[[local mesh_component = temp_actor:AddComponentByClass(static_mesh_component_c, false, zero_transform, false)
        if mesh_component ~= nil then
            mesh_component:SetStaticMesh(sphere_static_mesh)
            mesh_component:SetVisibility(true)
            mesh_component:SetHiddenInGame(false)
            temp_actor:FinishAddComponent(mesh_component, false, zero_transform)
            --mesh_component:SetWorldScale3D({X=0.5, Y=0.5, Z=0.5})  -- Adjust scale as needed
        end]]

        --[[local sphere_component = temp_actor:AddComponentByClass(sphere_component_c, false, zero_transform, false)
        if sphere_component ~= nil then
            sphere_component:SetVisibility(true)
            sphere_component:SetHiddenInGame(false)
            temp_actor:FinishAddComponent(sphere_component, false, zero_transform)
            sphere_component:SetSphereRadius(10.0)
        end]]
    end
    

    if temp_actor ~= nil and pawn ~= nil then
        local weapon = pawn.CurrentlyEquippedWeapon

        if weapon ~= nil then
            local muzzle = weapon.MuzzleFlashPointLight

            if muzzle ~= nil then
                --local muzzle_location = muzzle:K2_GetComponentLocation()
                --local muzzle_rotation = muzzle:K2_GetComponentRotation()
                local mesh = weapon.GunMesh

                --[[local mesh_attach_parent = mesh:GetAttachParent()
                local skeletal_mesh = mesh_attach_parent.SkeletalMesh
                local socket = skeletal_mesh:FindSocket("Fire_FX_Slot")]]

                local muzzle_location = mesh:GetSocketLocation(fire_fx_slot_fname)
                local muzzle_rotation = mesh:GetSocketRotation(fire_fx_slot_fname)
                --print("Muzzle rotation: " .. tostring(muzzle_rotation.Pitch) .. ", " .. tostring(muzzle_rotation.Yaw) .. ", " .. tostring(muzzle_rotation.Roll))

                local muzzle_dir = kismet_math_library:Conv_RotatorToVector(muzzle_rotation)
                local muzzle_end = muzzle_location + (muzzle_dir * 8192.0)

                --print(" Muzzle dir: " .. tostring(muzzle_dir.X) .. ", " .. tostring(muzzle_dir.Y) .. ", " .. tostring(muzzle_dir.Z))
                --print(" Muzzle rotation now: " .. tostring(muzzle_rotation.Pitch) .. ", " .. tostring(muzzle_rotation.Yaw) .. ", " .. tostring(muzzle_rotation.Roll))

                local ignore_actors = {pawn, weapon, temp_actor}
                
                local hit = kismet_system_library:SphereTraceSingle(world, muzzle_location, muzzle_end, 1.0, 15, true, ignore_actors, 0, reusable_hit_result, true, zero_color, zero_color, 1.0)

                if hit then
                    local hit_result = reusable_hit_result
                    --local hit_result_location = hit_result.ImpactPoint
                    local hit_result_location = (muzzle_location + (muzzle_dir * hit_result.Distance))
                    local hit_result_normal = hit_result.ImpactNormal
                    local unquantized_location = Vector3f.new(hit_result_location.X, hit_result_location.Y, hit_result_location.Z)
                    --local normal = Vector3f.new(hit_result_normal.X, hit_result_normal.Y, hit_result_normal.Z)
                    --local rot = kismet_math_library:MakeRotFromX(normal)

                    muzzle_rotation.Yaw = muzzle_rotation.Yaw + 180.0
                    muzzle_rotation.Pitch = -muzzle_rotation.Pitch
                    muzzle_rotation.Roll = 0.0

                    local delta = muzzle_location - last_impact_pos 
                    --local delta_length = delta:length()
                    
                    local len = math.max(1.0, delta:length() * 0.002)
                    temp_actor:SetActorScale3D(Vector3f.new(len, len, len))

                    last_impact_pos = last_impact_pos:lerp(unquantized_location, 1.0)
                    temp_actor:K2_SetActorLocationAndRotation(last_impact_pos, muzzle_rotation, true, zero_hit_result, true)
                else
                    temp_actor:K2_SetActorLocationAndRotation(muzzle_location, muzzle_rotation, false, zero_hit_result, false)
                end
            end
        end
    end
end)

uevr.sdk.callbacks.on_script_reset(function()
    print("Resetting hello_world.lua")

    reset_temp_actor()
end)

uevr.sdk.callbacks.on_xinput_get_state(function(retval, user_index, state)
    local gamepad = state.Gamepad

    local buttons = gamepad.wButtons
    local left_trigger = gamepad.bLeftTrigger
    local right_trigger = gamepad.bRightTrigger

    --print("Buttons: " .. tostring(buttons) .. ", Left trigger: " .. tostring(left_trigger) .. ", Right trigger: " .. tostring(right_trigger))
end)