
#pragma once
#include <cstdint>
#include <string>
#include <vector>


class UObject;
class UClass;
class ULevel;
class AActor;
class APawn;
class AHUD;
class APlayerCameraManager;
class UCheatManager;
class UPlayerInput;
class UInputComponent;
class UTouchInterface;
class ASpectatorPawn;
class UNetConnection;
class UMaterialInterface;
class UCameraAnim;
class UCameraShake;
class UForceFeedbackEffect;
class USoundBase;
class ULocalMessage;
class AEmitterCameraLensEffectBase;
class UUserWidget;
class UInterpTrackInstDirector;
class UGameViewportClient;
class UGameInstance;
class ULocalPlayer;
class UPlayer;
class UCharacterMovementComponent;
class ASCCharacter;
class UCameraComponent;
class UILLDynamicCameraComponent;
class ABase_Counselor;
class USCFearComponent;

typedef unsigned char uint8;
typedef unsigned int uint32;

struct FRotator {
    float Pitch, Yaw, Roll;
    FRotator() : Pitch(0), Yaw(0), Roll(0) {}
    FRotator(float p, float y, float r) : Pitch(p), Yaw(y), Roll(r) {}
};



typedef unsigned char uint8;
typedef unsigned int uint32;



struct FName {
    int32_t ComparisonIndex;
    int32_t Number;
    FName() : ComparisonIndex(0), Number(0) {}
    FName(int32_t idx, int32_t num) : ComparisonIndex(idx), Number(num) {}
};

#define OFFSET_GAMEVIEWPORT 0x750
#define OFFSET_WORLD 0x80
#define OFFSET_OWNINGGAMEINSTANCE 0x140
#define OFFSET_LOCALPLAYERS 0x38
#define OFFSET_PLAYERCONTROLLER 0x30
#define OFFSET_PAWN 0x370
#define OFFSET_CHARACTERMOVEMENT 0x3D0
#define OFFSET_CAMERAMANAGER 0x400
#define OFFSET_FEARMANAGER 0x1690

template<typename T>
struct TArray {
    T* Data;
    int32_t Count;
    int32_t Max;

    T& operator[](int32_t index) {
        return Data[index];
    }

    int32_t Num() const {
        return Count;
    }
};

struct FVector {
    float X, Y, Z;

    FVector operator-(const FVector& other) const {
        return { X - other.X, Y - other.Y, Z - other.Z };
    }
};

struct FVector2D {
    float X, Y;
};


struct FString {
    wchar_t* Data;
    int32_t NumElements;
    int32_t MaxElements;
    FString() : Data(nullptr), NumElements(0), MaxElements(0) {}
};

struct FNameEntry {
    uint32_t Index;
    FNameEntry* HashNext;
    char AnsiName[1024];
};

struct TNameEntryArray {
    FNameEntry** Chunks[128];
    int32_t NumElements;
    int32_t NumChunks;

    bool IsValidIndex(int32_t index) const { return index >= 0 && index < NumElements; }

    FNameEntry const* GetById(int32_t index) const {
        if (!IsValidIndex(index)) return nullptr;
        return Chunks[index / 16384][index % 16384];
    }
};

extern TNameEntryArray* GNames;



struct UObject {
    void* VTable;
    int32_t ObjectFlags;
    int32_t InternalIndex;
    class UClass* Class; 
    int32_t NameIndex;
    int32_t NameNumber;
    class UObject* OuterPrivate;

    std::string GetName() const;
};


class AActor : public UObject {
public:
    char Pad_0[0x28];
    float CustomTimeDilation;
    uint8_t bHidden : 1;
    uint8_t bNetTemporary : 1;
    uint8_t bNetStartup : 1;
    uint8_t bOnlyRelevantToOwner : 1;
    uint8_t bAlwaysRelevant : 1;
    uint8_t bReplicateMovement : 1;
    uint8_t bTearOff : 1;
    uint8_t bExchangedRoles : 1;
    uint8_t bNetLoadOnClient : 1;
    uint8_t bNetUseOwnerRelevancy : 1;
    uint8_t bBlockInput : 1;
    uint8_t bAllowTickBeforeBeginPlay : 1;
    uint8_t bActorEnableCollision : 1;
    uint8_t bReplicates : 1;
    int RemoteRole;
    UObject* Owner;
    float InitialLifeSpan;
    int Role;
    int NetDormancy;
    int AutoReceiveInput;
    int32_t InputPriority;
    UObject* InputComponent;
    float NetCullDistanceSquared;
    int32_t NetTag;
    float NetUpdateFrequency;
    float MinNetUpdateFrequency;
    float NetPriority;
    uint8_t bAutoDestroyWhenFinished : 1;
    uint8_t bCanBeDamaged : 1;
    uint8_t bActorIsBeingDestroyed : 1;
    uint8_t bCollideWhenPlacing : 1;
    uint8_t bFindCameraComponentWhenViewTarget : 1;
    uint8_t bRelevantForNetworkReplays : 1;
    uint8_t bGenerateOverlapEventsDuringLevelStreaming : 1;
    uint8_t bCanBeInCluster : 1;
    uint8_t bAllowReceiveTickEventOnDedicatedServer : 1;
    uint8_t bActorSeamlessTraveled : 1;
    uint8_t bIgnoresOriginShifting : 1;
    uint8_t bEnableAutoLODGeneration : 1;
    int SpawnCollisionHandlingMethod;
    UObject* Instigator;
    TArray<UObject*> Children;
    UObject* RootComponent;
    TArray<UObject*> ControllingMatineeActors;
    TArray<UObject*> Layers;
    UObject* ParentComponent;
    TArray<UObject*> Tags;
    uint64_t HiddenEditorViews;
    TArray<UObject*> BlueprintCreatedComponents;
    TArray<UObject*> InstanceComponents;
    FVector K2_GetActorLocation() const;
};

class ULevel : public UObject {
public:
    char Pad_0[0x78];
    TArray<AActor*> Actors;
};

class UEngine : public UObject {
public:
    char Pad_0[OFFSET_GAMEVIEWPORT - 0x28];
    void* GameViewport;
};

class UGameViewportClient : public UObject {
public:
    char Pad_0[OFFSET_WORLD - 0x28];
    void* World;
};



class UWorld : public UObject {
public:
    char Pad_0[0x8];
    ULevel* PersistentLevel;
    char Pad_1[0xC0];
    void* GameState;
    char Pad_2[0x40];
    void* OwningGameInstance;
};

class UGameInstance : public UObject {
public:
    char Pad_0[OFFSET_LOCALPLAYERS - 0x28];
    TArray<void*> LocalPlayers;
};

class ULocalPlayer : public UObject {
public:
    char Pad_0[OFFSET_PLAYERCONTROLLER - 0x28];
    void* PlayerController;
};


class UPlayer : public UObject {
public:
    TArray<UObject*> ComponentTags;
    TArray<UObject*> AssetUserData;
    uint8_t BitPad_A8_0 : 3;
    uint8_t bReplicates : 1;
    uint8_t bNetAddressable : 1;
    uint8_t BitPad_A8_5 : 3;
    uint8_t BitPad_A9_0 : 7;
    uint8_t bAutoActivate : 1;
    uint8_t bIsActive : 1;
    uint8_t bEditableWhenInherited : 1;
    uint8_t BitPad_AA_2 : 1;
    uint8_t bCanEverAffectNavigation : 1;
    uint8_t BitPad_AA_4 : 2;
    uint8_t bIsEditorOnly : 1;
    uint8_t Pad_AB[0x1];
    int CreationMethod;
    uint8_t Pad_AD[0x3];
    TArray<UObject*> UCSModifiedProperties;
    uint8_t Pad_E0[0x10];
};

class APawn : public AActor {
public:
    UObject* PhysicsVolume;
    UObject* AttachParent;
    TArray<UObject*> AttachChildren;
    TArray<UObject*> ClientAttachedChildren;
    uint8_t Pad_130[0x2C];
    FVector RelativeLocation;
    FVector RelativeRotation;
    FVector RelativeScale3D;
    uint8_t Pad_180[0x30];
    FVector ComponentVelocity;
    uint8_t bComponentToWorldUpdated : 1;
    uint8_t bAbsoluteLocation : 1;
    uint8_t bAbsoluteRotation : 1;
    uint8_t bAbsoluteScale : 1;
    uint8_t bVisible : 1;
    uint8_t bHiddenInGame : 1;
    uint8_t bShouldUpdatePhysicsVolume : 1;
    uint8_t bBoundsChangeTriggersStreamingDataRebuild : 1;
    uint8_t bUseAttachParentBound : 1;
    uint8_t BitPad_1BD_1 : 4;
    uint8_t bAbsoluteTranslation : 1;
    int Mobility;
    int DetailMode;
    uint8_t Pad_1C0[0x40];
    uint8_t Pad_210[0x80];
};

class UInterpTrackInstDirector : public UObject {
public:
    UObject* ParticleSystemComponent;
    uint8_t bDestroyOnSystemFinish : 1;
    uint8_t bPostUpdateTickGroup : 1;
    uint8_t bCurrentlyActive : 1;
    uint8_t Pad_371[0x7];
};

class AHUD : public AActor {
public:
    
};



class APlayerCameraManager : public UObject {
public:
    char Pad_0[0x380 - 0x28];
    float DefaultFOV;
};

class UCheatManager : public UObject {
public:
    
};

class UPlayerInput : public UObject {
public:
    
};

class UInputComponent : public UObject {
public:
    
};

class UTouchInterface : public UObject {
public:
    
};

class ASpectatorPawn : public APawn {
public:
    
};

class UNetConnection : public UObject {
public:
    
};

class UMaterialInterface : public UObject {
public:
    
};

class UCameraAnim : public UObject {
public:
    
};

class UCameraShake : public UObject {
public:
    
};

class UForceFeedbackEffect : public UObject {
public:
    
};

class USoundBase : public UObject {
public:
    
};

class ULocalMessage : public UObject {
public:
    
};

class AEmitterCameraLensEffectBase : public AActor {
public:
    
};

class UUserWidget : public UObject {
public:
    
};




class APlayerController : public UObject {
public:
    char Pad_0[0x3D0 - 0x28];
    UPlayer* Player;
    char Pad_1[0x8];
    APawn* AcknowledgedPawn;
    UInterpTrackInstDirector* ControllingDirTrackInst;
    char Pad_2[0x8];
    AHUD* MyHUD;
    APlayerCameraManager* PlayerCameraManager;
    TArray<APlayerCameraManager*> PlayerCameraManagerClass;
    bool bAutoManageActiveCameraTarget;
    char Pad_3[0x3];
    FRotator TargetViewRotation;
    char Pad_4[0xC];
    float SmoothTargetViewRotationSpeed;
    TArray<AActor*> HiddenActors;
    TArray<void*> HiddenPrimitiveComponents;
    char Pad_5[0x4];
    float LastSpectatorStateSynchTime;
    FVector LastSpectatorSyncLocation;
    FRotator LastSpectatorSyncRotation;
    int32_t ClientCap;
    char Pad_6[0x4];
    UCheatManager* CheatManager;
    TArray<UCheatManager*> CheatClass;
    UPlayerInput* PlayerInput;
    TArray<void*> ActiveForceFeedbackEffects;
    char Pad_7[0x90];
    uint8_t bPlayerIsWaiting : 1;
    char Pad_8[0x3];
    uint8_t NetPlayerIndex;
    char Pad_9[0x3B];
    UNetConnection* PendingSwapConnection;
    UNetConnection* NetConnection;
    char Pad_10[0xC];
    float InputYawScale;
    float InputPitchScale;
    float InputRollScale;
    uint8_t bShowMouseCursor : 1;
    uint8_t bEnableClickEvents : 1;
    uint8_t bEnableTouchEvents : 1;
    uint8_t bEnableMouseOverEvents : 1;
    uint8_t bEnableTouchOverEvents : 1;
    uint8_t bForceFeedbackEnabled : 1;
    char Pad_11[0x3];
    float ForceFeedbackScale;
    TArray<void*> ClickEventKeys;
    uint8_t DefaultMouseCursor;
    uint8_t CurrentMouseCursor;
    uint8_t DefaultClickTraceChannel;
    uint8_t CurrentClickTraceChannel;
    float HitResultTraceDistance;
    char Pad_12[0x80];
    UInputComponent* InactiveStateInputComponent;
    uint8_t bShouldPerformFullTickWhenPaused : 1;
    char Pad_13[0x17];
    UTouchInterface* CurrentTouchInterface;
    char Pad_14[0x40];
    ASpectatorPawn* SpectatorPawn;
    FVector SpawnLocation;
    char Pad_15[0x4];
    bool bIsLocalPlayerController;
    char Pad_16[0x1];
    uint16_t SeamlessTravelCount;
    uint16_t LastCompletedSeamlessTravelCount;
    char Pad_17[0x2];

    bool ProjectWorldLocationToScreen(const FVector& WorldLocation, FVector2D* ScreenLocation, bool bPlayerViewportRelative);
};

static enum EBoneIndex : int {
    Head = 51,
    Neck = 46,
    Neck2 = 45,
    InnerShoulder_L = 5,
    InnerShoulder_R = 25,


    RightShoulder = 26,
    RightElbow = 27,
    RightWrist = 28,

    LeftShoulder = 6,
    LeftElbow = 7,
    LeftWrist = 8,

    Spine = 4,
    Pelvis = 3,
    Dick = 1,

    Rightknee = 53,
    RightAnkle = 54,
    RightFeet = 66,

    Leftknee = 48,
    LeftAnkle = 49,
    LeftFeet = 50,

    
    
};


class ACharacter : public UObject {
public:
    char Pad_0[OFFSET_CHARACTERMOVEMENT - 0x28];
    void* CharacterMovement;
};

class ASCCharacter : public UObject {
public:
    char Pad_0[0x0E00 - 0x28];
    float MinInteractHoldTime;
    char Pad_1[0x0E98 - 0x0E00 - 0x4];
    bool bInStun;
    char Pad_2[OFFSET_FEARMANAGER - 0x0E98 - 0x1];
    void* FearManager;
};

class UCharacterMovementComponent {
public:
    char Pad_0[0x1AC];
    uint8_t MovementMode; 
    char Pad_1[0x27];
    float MaxWalkSpeed; 
    float MaxWalkSpeedCrouched;
    char Pad_1DC[0x544];
    float MaxSprintSpeed;
    float MaxRunSpeed;
    float MaxSlowRunSpeed;
    float MaxCombatSpeed;
};






struct FMinimalViewInfo {
    FVector Location;
    FRotator Rotation;
    float FOV;
};

struct FCameraCacheEntry {
    float Timestamp; 
    char pad_0004[0x000C];
    FMinimalViewInfo POV; 
};

struct FTViewTarget {
    void* Target; 
    char pad_0008[0x0008];
    FMinimalViewInfo POV; 
};

class UCameraComponent : public UObject {
public:
    char Pad_0[0x0290 - 0x28];
    float FieldOfView; 
};

class UILLDynamicCameraComponent : public UObject {
public:
    char Pad_0[0x0298 - 0x28];
    UCameraComponent* Camera; 
};

class ABase_Counselor : public UObject {
public:
    char Pad_0[0x1D28 - 0x28];
    UILLDynamicCameraComponent* ChaseCamera; 
};

class APlayerCameraManager_Full : public UObject {
public:
    char Pad_0[0x0380 - 0x28];
    float DefaultFOV; 
    char Pad_1[0x005C];
    FCameraCacheEntry CameraCache; 
    char Pad_2[0x0AC0];
    FTViewTarget ViewTarget; 
};

struct FFloat_NetQuantize {
    float Value;
    char Pad[0x8];
};

class USCFearComponent : public UObject {
public:
    char Pad_0[0x0130 - 0x28];
    FFloat_NetQuantize FearAmount;
};


struct UField {
    void* VTable;
    int32_t ObjectFlags;
    int32_t InternalIndex;
    void* ClassPrivate;
    int32_t NameIndex;
    int32_t NameNumber;
    UField* OuterPrivate;
    UField* Next;
    std::string GetName() const {
        if (!GNames) return "NoGNames";
        if (!GNames->IsValidIndex(NameIndex)) return "NullEntry";
        auto entry = GNames->GetById(NameIndex);
        if (!entry) return "NullEntry";
        return std::string(entry->AnsiName);
    }
};


struct UStruct : public UField {
    UStruct* Super;
    UField* Children;
    int32_t Size;
    int16_t MinAlignment;
    uint8 Pad_46[0x42];
};



class UClass : public UStruct {
public:
    uint8 Pad_88[0x30];
    enum class EClassCastFlags {
        None = 0,
        Interface = 0x00000001,
        
    } CastFlags;
    uint8 Pad_C0[0x38];
    class UObject* DefaultObject;
    uint8 Pad_100[0xF8];

    class UFunction* GetFunction(const char* ClassName, const char* FuncName) const;

    static UClass* StaticClass() {
        
        return nullptr;
    }
    static const class FName& StaticName() {
        
        static FName name;
        return name;
    }
    static UClass* GetDefaultObj() {
        
        return nullptr;
    }
};



class UFunction : public UStruct {
public:
    using FNativeFuncPtr = void (*)(void* Context, void* TheStack, void* Result);
    uint32 FunctionFlags;
    uint8 Pad_8C[0x24];
    FNativeFuncPtr ExecFunction;

    std::string GetName() const {
        return UField::GetName();
    }

    static UClass* StaticClass() { return nullptr; }
    static const class FName& StaticName() { static FName name; return name; }
    static UFunction* GetDefaultObj() { return nullptr; }
};



extern UEngine** GEngine;
extern UWorld** GWorld;

namespace Engine {
    bool Initialize();
    bool IsInGame();
    UWorld* GetWorld();
    ULocalPlayer* GetLocalPlayer();
    APlayerController* GetPlayerController();
    APlayerController* GetLocalPlayerController(); 
    ACharacter* GetLocalCharacter();
    ASCCharacter* GetSCCharacter();
    UCharacterMovementComponent* GetMovementComponent();
    APlayerCameraManager* GetCameraManager();
    USCFearComponent* GetFearComponent();
    void SetPlayerLookInput(bool bIgnore);

}






