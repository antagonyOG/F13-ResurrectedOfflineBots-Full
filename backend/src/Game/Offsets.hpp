#pragma once
#include <cstdint>

namespace Offsets {
    
    constexpr size_t Actor_RootComponent = 0x0160;
    constexpr size_t Actor_BlueprintCreatedComponents = 0x0340;
    constexpr size_t Actor_bHidden = 0x0084; 

    
    constexpr size_t Scene_RelativeLocation = 0x015C;
    constexpr size_t Scene_ComponentToWorld = 0x0180;

    
    constexpr size_t FTransform_Translation = 0x0010;

    
    constexpr size_t Character_Movement = 0x03D0;
    constexpr size_t Character_Mesh = 0x03C8;

    
    constexpr size_t Movement_MovementMode = 0x01AC;

    
    constexpr size_t Controller_Pawn = 0x0370;
    constexpr size_t PlayerController_PlayerState = 0x0388;
    constexpr size_t Controller_ControlRotation = 0x0288; 

    
    constexpr size_t Pawn_PlayerState = 0x0388;

    
    constexpr size_t PlayerState_PlayerName = 0x0370;

    
    constexpr size_t ASCCharacter_Health = 0x0E48;
    constexpr size_t ASCCharacter_MaxHealth = 0x0E54;

    
    constexpr size_t World_GameState = 0x00F8;
    constexpr size_t World_PersistentLevel = 0x0030;

    
    constexpr size_t Level_Actors = 0x00A0;

    
    constexpr ptrdiff_t Light_Intensity = 0x02A4; 
    constexpr ptrdiff_t Light_Flags = 0x02AC; 
    constexpr ptrdiff_t Primitive_Flags = 0x02A9; 

    
    constexpr ptrdiff_t World_Levels = 0x0110; 
    constexpr ptrdiff_t Actor_InstanceComponents = 0x0350; 

    
    constexpr size_t LocalPlayer_ViewportClient = 0x0058; 

    
    
    constexpr size_t GameState_PlayerArray = 0x0380;

    
    constexpr size_t Counselor_Nameplate = 0x1198;
    constexpr size_t Counselor_LightMesh = 0x11A0;
    constexpr size_t Counselor_CurrentStamina = 0x12E8;
    constexpr size_t Counselor_StaminaMax = 0x12F4;

    
    constexpr size_t Killer_GrabbedMeshOffset = 0x1198;

    
    constexpr size_t DamageCharacterMod = 0x12F4;
    constexpr size_t StunDamageMod = 0x12F0;
    constexpr size_t DismemberChance = 0x12E8;
}
