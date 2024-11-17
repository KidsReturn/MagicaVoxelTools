#include "Importers/MagicaVox.h"
#include "VoxelAssets/VoxelDataAssetData.inl"

#define OGT_VOX_IMPLEMENTATION
#include "ogt_vox.h"

#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"

static uint8 ImportThreads = 12;
static TSharedPtr<FMagicaVoxelQueuedThreadPool> ImportPool = nullptr;
static TAutoConsoleVariable<int32> CVarLogImport(TEXT("voxel.LogImport"),0,TEXT("enable import logs"),ECVF_Default);
static TAutoConsoleVariable<int32> CVarDebugSurfaceLevel(TEXT("voxel.DebugSurfaceLevel"), 70,TEXT("log index with z = surface level"),ECVF_Default);

FMagicaVoxelQueuedThreadPool::FQueuedThread::FQueuedThread(FMagicaVoxelQueuedThreadPool* Pool, const FString& ThreadName, uint32 StackSize, EThreadPriority ThreadPriority)
	: ThreadName(ThreadName)
	, ThreadPool(Pool)
	, DoWorkEvent(FPlatformProcess::GetSynchEventFromPool()) // Create event BEFORE thread
	, TimeToDie(false) // BEFORE creating thread
	, Thread(FRunnableThread::Create(this, *ThreadName, StackSize, ThreadPriority, FPlatformAffinity::GetPoolThreadMask()))
{
	check(Thread.IsValid());
}

FMagicaVoxelQueuedThreadPool::FQueuedThread::~FQueuedThread()
{
	// Tell the thread it needs to die
	TimeToDie = true;
	// Trigger the thread so that it will come out of the wait state if
	// it isn't actively doing work
	DoWorkEvent->Trigger();
	// If waiting was specified, wait the amount of time. If that fails,
	// brute force kill that thread. Very bad as that might leak.
	Thread->WaitForCompletion();
	// Clean up the event
	FPlatformProcess::ReturnSynchEventToPool(DoWorkEvent);
}

uint32 FMagicaVoxelQueuedThreadPool::FQueuedThread::Run()
{
	while (!TimeToDie)
	{
		// We need to wait for shorter amount of time
		bool bContinueWaiting = true;
		while (bContinueWaiting)
		{
			VOXEL_ASYNC_VERBOSE_SCOPE_COUNTER("FVoxelQueuedThread::Run.WaitForWork");

			// Wait for some work to do
			bContinueWaiting = !DoWorkEvent->Wait(10);
		}

		if (!TimeToDie)
		{
			IMagicaVoxelQueuedWork* LocalQueuedWork = ThreadPool->ReturnToPoolOrGetNextJob(this);

			while (LocalQueuedWork)
			{
				//const FName Name = LocalQueuedWork->Name;

				//const double StartTime = FPlatformTime::Seconds();

				LocalQueuedWork->DoThreadedWork();
				// IMPORTANT: LocalQueuedWork should be considered as deleted after this line

				//const double EndTime = FPlatformTime::Seconds();

				LocalQueuedWork = ThreadPool->ReturnToPoolOrGetNextJob(this);
			}
		}
	}
	return 0;
}

FMagicaVoxelQueuedThreadPool::FMagicaVoxelQueuedThreadPool(uint32 NumThreads, uint32 StackSize, EThreadPriority ThreadPriority)
{
	AllThreads.Reserve(NumThreads);
	for (uint32 ThreadIndex = 0; ThreadIndex < NumThreads; ThreadIndex++)
	{
		const FString Name = FString::Printf(TEXT("MagicaVoxelThread %d"), ThreadIndex);
		AllThreads.Add(MakeUnique<FQueuedThread>(this, Name, StackSize, ThreadPriority));
	}
	//
	QueuedThreads.Reserve(NumThreads);
	for (auto& Thread : AllThreads)
	{
		QueuedThreads.Add(Thread.Get());
	}
}

FMagicaVoxelQueuedThreadPool::~FMagicaVoxelQueuedThreadPool()
{
	AbandonAllTasks();
}

bool FMagicaVoxelQueuedThreadPool::IsWorking()
{
	FScopeLock Lock(Section);
	return AllThreads.Num() != QueuedThreads.Num();
}

void FMagicaVoxelQueuedThreadPool::AddQueuedWork(IMagicaVoxelQueuedWork* InQueuedWork)
{
	check(IsInGameThread());
	check(InQueuedWork);

	if (TimeToDie)
	{
		InQueuedWork->Abandon();
		return;
	}

	FQueuedWorkInfo WorkInfo(InQueuedWork);
	//
	Section.Lock();
	QueuedWorks.push(WorkInfo);
	for (auto* QueuedThread : QueuedThreads)
	{
		QueuedThread->DoWorkEvent->Trigger();
	}
	QueuedThreads.Reset();
	Section.Unlock();
}

void FMagicaVoxelQueuedThreadPool::AddQueuedWorks(const TArray<IMagicaVoxelQueuedWork*>& InQueuedWorks)
{
	check(IsInGameThread());

	if (TimeToDie)
	{
		for (auto* InQueuedWork : InQueuedWorks)
		{
			InQueuedWork->Abandon();
		}
		return;
	}
	//
	FScopeLock Lock(Section);
	for (auto* InQueuedWork : InQueuedWorks)
	{
		QueuedWorks.emplace(InQueuedWork);
	}
	for (auto* QueuedThread : QueuedThreads)
	{
		QueuedThread->DoWorkEvent->Trigger();
	}
	QueuedThreads.Reset();
}

void FMagicaVoxelQueuedThreadPool::AbandonAllTasks()
{
	if (TimeToDie)
	{
		return;
	}

	{
		FScopeLock Lock(Section);
		TimeToDie = true;
		// Clean up all queued objects
		while (!QueuedWorks.empty())
		{
			QueuedWorks.top().Work->Abandon();
			QueuedWorks.pop();
		}
	}
	// Wait for all threads to finish up
	while (true)
	{
		{
			FScopeLock Lock(Section);
			if (AllThreads.Num() == QueuedThreads.Num())
			{
				break;
			}
		}
		FPlatformProcess::Sleep(0.0f);
	}
}

IMagicaVoxelQueuedWork* FMagicaVoxelQueuedThreadPool::ReturnToPoolOrGetNextJob(FQueuedThread* InQueuedThread)
{
	check(InQueuedThread);

	FScopeLock Lock(Section);

	if (!QueuedWorks.empty())
	{
		auto* Work = QueuedWorks.top().Work;
		QueuedWorks.pop();
		check(Work);
		return Work;
	}
	else
	{
		QueuedThreads.Add(InQueuedThread);
		return nullptr;
	}
}

TSharedRef<FMagicaVoxelQueuedThreadPool, ESPMode::ThreadSafe> FMagicaVoxelQueuedThreadPool::Create(int32 NumThreads, uint32 StackSize, EThreadPriority ThreadPriority)
{
	const auto Pool = TSharedRef<FMagicaVoxelQueuedThreadPool, ESPMode::ThreadSafe>(new FMagicaVoxelQueuedThreadPool(NumThreads, StackSize, ThreadPriority));

	TFunction<void()> ShutdownCallback = [WeakPool = TWeakPtr<FMagicaVoxelQueuedThreadPool, ESPMode::ThreadSafe>(Pool)]()
	{
		auto PoolPtr = WeakPool.Pin();
		if (PoolPtr.IsValid())
		{
			PoolPtr->AbandonAllTasks();
		}
	};
	FTaskGraphInterface::Get().AddShutdownCallback(ShutdownCallback);

	return Pool;
}

bool MagicaVox::ImportToAsset(const FString& Filename, FVoxelDataAssetData& Asset, const FVoxelDataAssetImportSettings_MagicaVox& InSetting)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Error when opening the file")));
		return false;
	}

	const ogt_vox_scene* Scene = ogt_vox_read_scene(Bytes.GetData(), Bytes.Num());
	if (!Scene)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Error when decoding the scene")));
		return false;
	}

	ON_SCOPE_EXIT
	{
		ogt_vox_destroy_scene(Scene);
	};

	const auto Models = TArrayView<const ogt_vox_model*>(Scene->models, Scene->num_models);
	if (Models.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("No models in the file")));
		return false;
	}
	
	// import all instance with transform
	if (!ImportPool.IsValid())
	{
		ImportPool = FMagicaVoxelQueuedThreadPool::Create(ImportThreads, 1024 * 1024, EThreadPriority::TPri_Normal);
		check(ImportPool.IsValid());
	}
	TPair<FVoxelIntBox, TArray<uint8>> SceneData;
	if (MergeSceneData(Scene, SceneData))
	{
		ImportPool->AddQueuedWorks(FMagicaVoxImportWork::Create(Asset, SceneData, ImportThreads, InSetting));
		while (ImportPool->IsWorking())
		{
			FPlatformProcess::Sleep(0.0f);
		}
	}
	else
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("failed to merge scene"))));
		return false;
	}
	
	return true;
}

// import all instance with transform, modify marching cube value to render regular hexagon
bool MagicaVox::MergeSceneData(const ogt_vox_scene* InScene, TPair<FVoxelIntBox, TArray<uint8>>& OutData)
{
	if (InScene == nullptr)
	{
		return false;
	}

	const auto Instances = TArrayView<const ogt_vox_instance>(InScene->instances, InScene->num_instances);
	const auto Models = TArrayView<const ogt_vox_model*>(InScene->models, InScene->num_models);
	TMap<FVoxelIntBox, TArray<FUintVector4>> InstMap;
	FVoxelIntBox SceneBounds;
	for (const auto& Inst : Instances)
	{
		TPair<FVoxelIntBox, TArray<FUintVector4>> Temp;
		FMatrix44f Matrix(FPlane4f(Inst.transform.m00, Inst.transform.m01, Inst.transform.m02, Inst.transform.m03)
			, FPlane4f(Inst.transform.m10, Inst.transform.m11, Inst.transform.m12, Inst.transform.m13)
			, FPlane4f(Inst.transform.m20, Inst.transform.m21, Inst.transform.m22, Inst.transform.m23)
			, FPlane4f(Inst.transform.m30, Inst.transform.m31, Inst.transform.m32, Inst.transform.m33));
		if (Models.IsValidIndex(Inst.model_index) && UnifyModelData(Models[Inst.model_index], Matrix, Temp))
		{
			SceneBounds = SceneBounds + Temp.Key;
			InstMap.Add(MoveTemp(Temp));
		}
		else
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("failed to import model index[%d] at transofrom[%s]"), Inst.model_index, *Matrix.ToString())));
			return false;
		}
	}
	//
	TArray<uint8> VoxelData;
	VoxelData.SetNum(SceneBounds.Count());
	//
	ImportPool->AddQueuedWorks(FMagicaVoxMergeWork::Create(VoxelData, SceneBounds, InstMap));
	while (ImportPool->IsWorking())
	{
		FPlatformProcess::Sleep(0.0f);
	}

	OutData = TPair<FVoxelIntBox, TArray<uint8>>(MoveTemp(SceneBounds), MoveTemp(VoxelData));
	return true;
}

bool MagicaVox::UnifyModelData(const ogt_vox_model* InModel, const FMatrix44f& InMatrix, TPair<FVoxelIntBox, TArray<FUintVector4>>& OutData)
{
	if (InModel == nullptr || InModel->voxel_data == nullptr)
	{
		return false;
	}

	const uint32 SizeX = InModel->size_x;
	const uint32 SizeY = InModel->size_y;
	const uint32 SizeZ = InModel->size_z;
	const FVector4f LocalHalfSize = FVector4f(SizeX * 0.5f, SizeY * 0.5f, SizeZ * 0.5f, 0.f);
	FVector4f WorldHalfSize = InMatrix.TransformVector(LocalHalfSize);
	WorldHalfSize.X = FMath::Abs(WorldHalfSize.X);
	WorldHalfSize.Y = FMath::Abs(WorldHalfSize.Y);
	WorldHalfSize.Z = FMath::Abs(WorldHalfSize.Z);
	// main idea is the real center is not affected by rotation, shift it with desired rotation offset.
	// defaul axis, x = 1, y = 1, z = 1. 
	static const FVector4f DefaultVoxelPivotVec(FVector4f(-0.5f, -0.5f, -0.5f, 0.f));			
	const FVector4f LocalObjectPivotVec(FMath::Frac(LocalHalfSize.X), FMath::Frac(LocalHalfSize.Y), FMath::Frac(LocalHalfSize.Z), 0.f);
	const FVector4f LocalObjectCenterToOriginVoxelCenter(-FMath::Floor(LocalHalfSize.X) + 0.5f, -FMath::Floor(LocalHalfSize.Y) + 0.5f, -FMath::Floor(LocalHalfSize.Z) + 0.5f, 0.f);
	const FVector4f WorldObjectCenterPos(InMatrix.M[3][0], InMatrix.M[3][1], InMatrix.M[3][2], 1.f);
	const FVector4f WorldObjectPivotVec = InMatrix.TransformVector(LocalObjectPivotVec);
	const FVector4f WorldObjectCenterToOriginVoxelCenter = InMatrix.TransformVector(LocalObjectCenterToOriginVoxelCenter);
	//
	const FVector4f UnifiedOrigin = WorldObjectCenterPos + WorldObjectPivotVec - WorldHalfSize;
	const FVector4f UnifiedTranslation = WorldObjectCenterPos + WorldObjectCenterToOriginVoxelCenter + DefaultVoxelPivotVec;
	FMatrix44f IndexMatrix(InMatrix);
	IndexMatrix.M[3][0] = UnifiedTranslation.X - UnifiedOrigin.X;
	IndexMatrix.M[3][1] = UnifiedTranslation.Y - UnifiedOrigin.Y;
	IndexMatrix.M[3][2] = UnifiedTranslation.Z - UnifiedOrigin.Z;
	//
	FVoxelIntBox Bounds(FIntVector(UnifiedOrigin.X, UnifiedOrigin.Y, UnifiedOrigin.Z), FIntVector(UnifiedOrigin.X + WorldHalfSize.X * 2, UnifiedOrigin.Y + WorldHalfSize.Y * 2, UnifiedOrigin.Z + WorldHalfSize.Z * 2));
	TArray<FUintVector4> Data;
	for (uint32 Z = 0; Z < SizeZ; Z++)
	{
		for (uint32 Y = 0; Y < SizeY; Y++)
		{
			for (uint32 X = 0; X < SizeX; X++)
			{
				const uint32 OldIndex = X + SizeX * Y + SizeX * SizeY * Z;
				const uint8 Voxel = InModel->voxel_data[OldIndex];
				FVector4f NewIndex = IndexMatrix.TransformPosition(FVector4f(X, Y, Z, 1.f));
				Data.Emplace(NewIndex.X, NewIndex.Y, NewIndex.Z, Voxel);
			}
		}
	}

	OutData = TPair<FVoxelIntBox, TArray<FUintVector4>>(MoveTemp(Bounds), MoveTemp(Data));
	return true;
}

TArray<IMagicaVoxelQueuedWork*> MagicaVox::FMagicaVoxImportWork::Create(FVoxelDataAssetData& InAssetData, const TPair<FVoxelIntBox, TArray<uint8>>& InSceneData, uint32 InNumThreads, const FVoxelDataAssetImportSettings_MagicaVox& InSetting)
{
	InSetting.InitForMultiThread();
	TArray<IMagicaVoxelQueuedWork*> Works;
	FIntVector Size = InSceneData.Key.Size();
	InAssetData.SetSize(FIntVector(Size.Y, Size.X, Size.Z), true, true);			// MagicaVoxe and UE use different coordination
	FIntVector PrevMin(0, 0, 0);
	uint32 Max = Size.GetMax();
	if (Max > InNumThreads)
	{
		uint32 EachSize = Max / InNumThreads;
		for (uint32 i = 1; i < InNumThreads; i++)
		{
			uint32 CurSize = EachSize * i;
			FIntVector TempMax(Max == Size.X ? CurSize : Size.X, Max == Size.Y ? CurSize : Size.Y, Max == Size.Z ? CurSize : Size.Z);
			Works.Add(new FMagicaVoxImportWork(InAssetData, InSceneData.Value, FVoxelIntBox(PrevMin, TempMax), Size, InSetting));
			PrevMin = FIntVector(Max == Size.X ? CurSize : 0, Max == Size.Y ? CurSize : 0, Max == Size.Z ? CurSize : 0);
		}
	}
	Works.Add(new FMagicaVoxImportWork(InAssetData, InSceneData.Value, FVoxelIntBox(PrevMin, Size), Size, InSetting));
	return MoveTemp(Works);
}

FVector MagicaVox::FMagicaVoxImportWork::GetCenter(const FVector& v, bool bDiagonal) const
{
#if 1 // [KidsReturn]
	const int32 halfHeight = Setting.GetHalfHeight();
	const int32 quarterWidth = floor(Setting.HalfWidth * 0.5);
	const int32 oneRowOffset = halfHeight;
	const int32 twoRowOffset = halfHeight * 2;
	const int32 oneColumnOffset = Setting.HalfWidth * 1.5;
	const int32 twoColumnOffset = Setting.HalfWidth * 3;
#endif
	float xStep = floor(v.X / twoColumnOffset);
	float yStep = floor(v.Y / twoRowOffset);
	float xDiagStep = floor((v.X + oneColumnOffset) / twoColumnOffset);
	float yDiagStep = floor((v.Y + oneRowOffset) / twoRowOffset);
	return bDiagonal ? FVector(xDiagStep * twoColumnOffset - quarterWidth, yDiagStep * twoRowOffset, v.Z) : FVector(xStep * twoColumnOffset + Setting.HalfWidth, yStep * twoRowOffset + halfHeight, v.Z);
}

int32 MagicaVox::FMagicaVoxImportWork::IsInbound(const FVector& c, const FVector& v) const
{
#if 1 // [KidsReturn]
	const int32 halfHeight = Setting.GetHalfHeight();
#endif
	// 0 = outside 1 = border 2 = inside 3 = center
	const FVector2D dist = FVector2D(v - c);
	if (dist.IsZero())
	{
		return 3;
	}
	const float x2c = abs(dist.X);
	const float y2c = abs(dist.Y);
	const float xDist = tan(30 * PI / 180) * y2c + 0.5;																// 0.5 for center of farest point
	const float xOffset = floor(xDist) + (FMath::Frac(xDist) > 0.5 && y2c != halfHeight ? 1 : 0);						// Y2C != halfHeight is because we are using cube to approximate regular hexagon, so the value is not exact, need clamp
	if (x2c <= Setting.HalfWidth - xOffset && y2c <= halfHeight)
	{
		return (x2c == Setting.HalfWidth - xOffset || y2c == halfHeight) ? 1 : 2;
	}
	else
	{
		return 0;
	}
}

int32 MagicaVox::FMagicaVoxImportWork::GetBorderClockPos(FVector& c, const FVector& v) const
{
	// here we figure out points shared by 3 hexagon : 1, 3, 5, 7, 9, 11. others are shared by 2 hexagon. P.S. base on flat-top
	// 0 means invalid.
	float b = IsInbound(c, v);
	if (b == 1)
	{
#if 1 // [KidsReturn]
		const int32 halfHeight = Setting.GetHalfHeight();
		const int32 quarterWidth = floor(Setting.HalfWidth * 0.5);
		int32 cp = 0;
#endif
		// on border we check diagonal
		const FVector2D dist = FVector2D(v - c);
		bool bTop = dist.Y >= 0;		// y == 0 doesn't have specific meaning
		bool bLeft = dist.X < 0;
		if (bTop)
		{
			if (dist.Y == halfHeight)
			{
				cp = abs(dist.X) == quarterWidth ? (bLeft ? 11 : 1) :  12;
			}
			else if (abs(dist.X) == Setting.HalfWidth && dist.Y == 0)
			{
				cp = bLeft ? 9 : 3;
			}
			else
			{
				cp = bLeft ? 10 : 2;
			}
		}
		else
		{
			if (dist.Y == -halfHeight)
			{
				cp = abs(dist.X) == quarterWidth ? (bLeft ? 7 : 5) :  6;
			}
			else if (abs(dist.X) == Setting.HalfWidth && dist.Y == 0)
			{
				cp = bLeft ? 9 : 3;
			}
			else
			{
				cp = bLeft ? 8 : 4;
			}
		}
#if 1 // [KidsReturn] base on pos above, validate center and convert if needed
		int32 Idx = c.X + SceneSize.X * c.Y + SceneSize.X * SceneSize.Y * c.Z;
		if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
		{
			return cp;
		}
		else
		{
			// shared by 3 hexagon : 1, 3, 5, 7, 9, 11. others are shared by 2 hexagon. P.S. base on flat-top
			if (cp == 2 || cp == 4 || cp == 8 || cp == 10)
			{
				return cp;
			}
			else
			{
				const int32 oneRowOffset = halfHeight;
				const int32 twoRowOffset = halfHeight * 2;
				const int32 oneColumnOffset = Setting.HalfWidth * 1.5;
				// top
				if (cp == 11 || cp == 12 || cp == 1)
				{
					Idx = c.X + SceneSize.X * (c.Y + twoRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X, c.Y + twoRowOffset, c.Z);
						return cp == 1 ? 5 : (cp == 11 ? 7 : 6);
					}
				}
				// top right
				if (cp >= 1 && cp <= 3)
				{
					Idx = (c.X + oneColumnOffset) + SceneSize.X * (c.Y + oneRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X + oneColumnOffset, c.Y + oneRowOffset, c.Z);
						return cp == 1 ? 9 : (cp == 3 ? 7 : 8);
					}
				}
				// bottom right
				if (cp >= 3 && cp <= 5)
				{
					Idx = (c.X + oneColumnOffset) + SceneSize.X * (c.Y - oneRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X + oneColumnOffset, c.Y - oneRowOffset, c.Z);
						return cp == 3 ? 11 : (cp == 5 ? 9 : 10);
					}
				}
				// bottom
				if (cp == 5 || cp == 6 || cp == 7)
				{
					Idx = c.X + SceneSize.X * (c.Y - twoRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X, c.Y - twoRowOffset, c.Z);
						return cp == 5 ? 1 : (cp == 7 ? 11 : 12);
					}
				}
				// bottom left
				if (cp >= 7 && cp <= 9)
				{
					Idx = (c.X - oneColumnOffset) + SceneSize.X * (c.Y - oneRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X - oneColumnOffset, c.Y - oneRowOffset, c.Z);
						return cp == 7 ? 3 : (cp == 9 ? 1 : 2);
					}
				}
				// top left
				if (cp >= 9 && cp <= 11)
				{
					Idx = (c.X - oneColumnOffset) + SceneSize.X * (c.Y + oneRowOffset) + SceneSize.X * SceneSize.Y * c.Z;
					if (MagicaData.IsValidIndex(Idx) && MagicaData[Idx] != 0)
					{
						c = FVector(c.X - oneColumnOffset, c.Y + oneRowOffset, c.Z);
						return cp == 9 ? 5 : (cp == 11 ? 3 : 4);
					}
				}
			}
		}
#endif
	}
	return 0;
}

void MagicaVox::FMagicaVoxImportWork::DoThreadedWork()
{
	for (int32 Z = Bounds.Min.Z; Z < Bounds.Max.Z; Z++)
	{
		const bool bShouldLog = CVarLogImport.GetValueOnAnyThread()> 0 && Z == CVarDebugSurfaceLevel.GetValueOnAnyThread();
		for (int32 Y = Bounds.Min.Y; Y < Bounds.Max.Y; Y++)
		{
			for (int32 X = Bounds.Min.X; X < Bounds.Max.X; X++)
			{
				const uint8& V = MagicaData[X + SceneSize.X * Y + SceneSize.X * SceneSize.Y * Z];
				if (V > 0)
				{
					FVoxelValue Value = FVoxelValue::Full();
					FVector Current = FVector(X, Y, Z);
					FVector Center = GetCenter(Current, false);
					if (IsInbound(Center, Current) != 1)
					{
						Center = GetCenter(Current, true);		// try diagonal
					}
					const int32 CP = GetBorderClockPos(Center, Current);
					if (CP != 0 && CP != 6 && CP != 12)
					{
						// towards right
						const int32 ShiftX = CP < 6 ? X + 1 : X - 1;		// cp 1-6 is towards right, 7-12 is towards left
						const int32 Idx = ShiftX + SceneSize.X * Y + SceneSize.X * SceneSize.Y * Z;
						if (MagicaData.IsValidIndex(Idx) && ShiftX >= 0 && ShiftX < SceneSize.X)
						{
							if (MagicaData[Idx] == 0)
							{
								const TPair<float, float>& Vox = Setting.VoxelValueByHeight[FMath::Abs(Current.Y - Center.Y)];	// first = inside, second = outside
								Value = FVoxelValue(Vox.Key);
								AssetData.SetValue(Y, ShiftX, Z, FVoxelValue(Vox.Value));
								if (bShouldLog) UE_LOG(LogTemp, Error, TEXT("linfei> DoWorkDual %d %d %d %d %.3f. CP[%d] Center[%s] Dist[%.1f]"), ShiftX, Y, Z, V, AssetData.GetValueUnsafe(Y, ShiftX, Z).ToFloat(), CP, *Center.ToString(), FMath::Abs(Current.Y - Center.Y));
							}
						}
						else if (CP == 3 || CP == 9)
						{
							Value = FVoxelValue(0.f);
						}
					}
					AssetData.SetValue(Y, X, Z, Value);
					if (bShouldLog) UE_LOG(LogTemp, Error, TEXT("linfei> DoWorkFull %d %d %d %d %.3f. CP[%d] Center[%s] Dist[%.1f]"), X, Y, Z, V, AssetData.GetValueUnsafe(Y, X, Z).ToFloat(), CP, *Center.ToString(), FMath::Abs(Current.Y - Center.Y));
				}
				else 
				{
					 if (bShouldLog) UE_LOG(LogTemp, Error, TEXT("linfei> DoWorkEmpty %d %d %d %d. IsNull[%d] value[%.3f]"), X, Y, Z, V, AssetData.GetValueUnsafe(Y, X, Z).IsNull(), AssetData.GetValueUnsafe(Y, X, Z).ToFloat());
					if (AssetData.GetValueUnsafe(Y, X, Z).IsNull())
					{
						AssetData.SetValue(Y, X, Z, FVoxelValue::Empty());
					}
				}
				
				FVoxelMaterial Material(ForceInit);
				if (V > 0)
				{
					Material.SetSingleIndex(V - 1);			// MagicaVoxel index start from 1, we are starting from 0
				}
				AssetData.SetMaterial(Y, X, Z, Material);
			}
		}
	}

	delete this;
}

void MagicaVox::FMagicaVoxImportWork::Abandon()
{

}

TArray<IMagicaVoxelQueuedWork*> MagicaVox::FMagicaVoxMergeWork::Create(TArray<uint8>& InVoxelData, const FVoxelIntBox& InSceneBounds, const TMap<FVoxelIntBox, TArray<FUintVector4>>& InInstMap)
{
	TArray<IMagicaVoxelQueuedWork*> Works;
	FIntVector SceneSize = InSceneBounds.Size();
	for (const TPair<FVoxelIntBox, TArray<FUintVector4>>& Inst : InInstMap)
	{
		Works.Add(new FMagicaVoxMergeWork(InVoxelData, InSceneBounds, Inst));
	}
	return MoveTemp(Works);
}

void MagicaVox::FMagicaVoxMergeWork::DoThreadedWork()
{
	const FVoxelIntBox& Bounds = InstData.Key;
	FIntVector Origin = Bounds.Min - SceneBounds.Min;
	FIntVector SceneSize = SceneBounds.Size();
	for (const FUintVector4& Data : InstData.Value)
	{
		VoxelData[Origin.X + Data.X + SceneSize.X * (Origin.Y + Data.Y) + SceneSize.X * SceneSize.Y * (Origin.Z + Data.Z)] = Data.W;
	}

	delete this;
}

void MagicaVox::FMagicaVoxMergeWork::Abandon()
{

}