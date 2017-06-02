// CARLA, Copyright (C) 2017 Computer Vision Center (CVC)

#include "Carla.h"
#include "CarlaGameModeBase.h"

#include "Engine/PlayerStartPIE.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"

#include "CarlaGameInstance.h"
#include "CarlaGameState.h"
#include "CarlaHUD.h"
#include "CarlaPlayerState.h"
#include "CarlaSettings.h"
#include "CarlaVehicleController.h"
#include "Tagger.h"
#include "TaggerDelegate.h"

ACarlaGameModeBase::ACarlaGameModeBase(const FObjectInitializer& ObjectInitializer) :
  Super(ObjectInitializer),
  GameController(nullptr),
  PlayerController(nullptr)
{
  PrimaryActorTick.bCanEverTick = true;
  PrimaryActorTick.TickGroup = TG_PrePhysics;
  bAllowTickBeforeBeginPlay = false;

  PlayerControllerClass = ACarlaVehicleController::StaticClass();
  GameStateClass = ACarlaGameState::StaticClass();
  PlayerStateClass = ACarlaPlayerState::StaticClass();
  HUDClass = ACarlaHUD::StaticClass();

  TaggerDelegate = CreateDefaultSubobject<UTaggerDelegate>(TEXT("TaggerDelegate"));
}

void ACarlaGameModeBase::InitGame(
    const FString &MapName,
    const FString &Options,
    FString &ErrorMessage)
{
  Super::InitGame(MapName, Options, ErrorMessage);

  GameInstance = Cast<UCarlaGameInstance>(GetGameInstance());
  checkf(
      GameInstance != nullptr,
      TEXT("GameInstance is not a UCarlaGameInstance, did you forget to set it in the project settings?"));
  GameInstance->InitializeGameControllerIfNotPresent(MockGameControllerSettings);
  GameController = &GameInstance->GetGameController();
  GameController->Initialize(GameInstance->GetCarlaSettings());
  GameInstance->GetCarlaSettings().LogSettings();

  if (TaggerDelegate != nullptr) {
    TaggerDelegate->RegisterSpawnHandler(GetWorld());
  } else {
    UE_LOG(LogCarla, Error, TEXT("Missing TaggerDelegate!"));
  }

  if (DynamicWeatherClass != nullptr) {
    DynamicWeather = GetWorld()->SpawnActor<ADynamicWeather>(DynamicWeatherClass);
  }

  if (VehicleSpawnerClass != nullptr) {
    VehicleSpawner = GetWorld()->SpawnActor<AVehicleSpawnerBase>(VehicleSpawnerClass);
  }

  if (WalkerSpawnerClass != nullptr) {
    WalkerSpawner = GetWorld()->SpawnActor<AWalkerSpawnerBase>(WalkerSpawnerClass);
  }
}

void ACarlaGameModeBase::RestartPlayer(AController* NewPlayer)
{
  check(NewPlayer != nullptr);
  TArray<APlayerStart *> UnOccupiedStartPoints;
  APlayerStart *PlayFromHere = FindUnOccupiedStartPoints(NewPlayer, UnOccupiedStartPoints);
  if (PlayFromHere != nullptr) {
    RestartPlayerAtPlayerStart(NewPlayer, PlayFromHere);
    RegisterPlayer(*NewPlayer);
    return;
  } else if (UnOccupiedStartPoints.Num() > 0u) {
    check(GameController != nullptr);
    APlayerStart *StartSpot = GameController->ChoosePlayerStart(UnOccupiedStartPoints);
    if (StartSpot != nullptr) {
      RestartPlayerAtPlayerStart(NewPlayer, StartSpot);
      RegisterPlayer(*NewPlayer);
      return;
    }
  }
  UE_LOG(LogCarla, Error, TEXT("No start spot found!"));
}

void ACarlaGameModeBase::BeginPlay()
{
  Super::BeginPlay();

  const auto &CarlaSettings = GameInstance->GetCarlaSettings();

  // Setup semantic segmentation if necessary.
  if (CarlaSettings.bSemanticSegmentationEnabled) {
    TagActorsForSemanticSegmentation();
    TaggerDelegate->SetSemanticSegmentationEnabled();
  }

  // Change weather.
  if (DynamicWeather != nullptr) {
    const auto *Weather = CarlaSettings.GetActiveWeatherDescription();
    if (Weather != nullptr) {
      UE_LOG(LogCarla, Log, TEXT("Changing weather settings to \"%s\""), *Weather->Name);
      DynamicWeather->SetWeatherDescription(*Weather);
      DynamicWeather->RefreshWeather();
    }
  } else {
    UE_LOG(LogCarla, Error, TEXT("Missing dynamic weather actor!"));
  }

  // Setup other vehicles.
  if (VehicleSpawner != nullptr) {
    VehicleSpawner->SetNumberOfVehicles(CarlaSettings.NumberOfVehicles);
  } else {
    UE_LOG(LogCarla, Error, TEXT("Missing vehicle spawner actor!"));
  }

  // Setup walkers.
  if (WalkerSpawner != nullptr) {
    WalkerSpawner->SetNumberOfWalkers(CarlaSettings.NumberOfPedestrians);
  } else {
    UE_LOG(LogCarla, Error, TEXT("Missing walker spawner actor!"));
  }

  GameController->BeginPlay();
}

void ACarlaGameModeBase::Tick(float DeltaSeconds)
{
  Super::Tick(DeltaSeconds);
  GameController->Tick(DeltaSeconds);
}

void ACarlaGameModeBase::RegisterPlayer(AController &NewPlayer)
{
  check(GameController != nullptr);
  AddTickPrerequisiteActor(&NewPlayer);
  GameController->RegisterPlayer(NewPlayer);
  PlayerController = &NewPlayer;
  AttachCaptureCamerasToPlayer(*PlayerController);
}

void ACarlaGameModeBase::AttachCaptureCamerasToPlayer(AController &Player)
{
  ACarlaVehicleController *Vehicle = Cast<ACarlaVehicleController>(&Player);
  if (Vehicle == nullptr) {
    UE_LOG(LogCarla, Warning, TEXT("Trying to add capture cameras but player is not a ACarlaVehicleController"));
    return;
  }
  const auto &Settings = GameInstance->GetCarlaSettings();
  const auto *Weather = Settings.GetActiveWeatherDescription();
  const FCameraPostProcessParameters *OverridePostProcessParameters = nullptr;
  if ((Weather != nullptr) && (Weather->bOverrideCameraPostProcessParameters)) {
    OverridePostProcessParameters = &Weather->CameraPostProcessParameters;
  }
  for (const auto &Item : Settings.CameraDescriptions) {
    Vehicle->AddSceneCaptureCamera(Item.Value, OverridePostProcessParameters);
  }
}

void ACarlaGameModeBase::TagActorsForSemanticSegmentation()
{
  check(GetWorld() != nullptr);
  ATagger::TagActorsInLevel(*GetWorld(), true);
}

APlayerStart *ACarlaGameModeBase::FindUnOccupiedStartPoints(
    AController *Player,
    TArray<APlayerStart *> &UnOccupiedStartPoints)
{
  APlayerStart* FoundPlayerStart = nullptr;
  UClass* PawnClass = GetDefaultPawnClassForController(Player);
  APawn* PawnToFit = PawnClass ? PawnClass->GetDefaultObject<APawn>() : nullptr;
  for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It) {
    APlayerStart* PlayerStart = *It;

    if (PlayerStart->IsA<APlayerStartPIE>()) {
      FoundPlayerStart = PlayerStart;
      break;
    } else {
      FVector ActorLocation = PlayerStart->GetActorLocation();
      const FRotator ActorRotation = PlayerStart->GetActorRotation();
      if (!GetWorld()->EncroachingBlockingGeometry(PawnToFit, ActorLocation, ActorRotation)) {
        UnOccupiedStartPoints.Add(PlayerStart);
      }
#if WITH_EDITOR
      else if (GetWorld()->FindTeleportSpot(PawnToFit, ActorLocation, ActorRotation)) {
        UE_LOG(
            LogCarla,
            Warning,
            TEXT("Player start cannot be used, occupied location: %s"),
            *PlayerStart->GetActorLocation().ToString());
      }
#endif // WITH_EDITOR
    }
  }
  return FoundPlayerStart;
}
