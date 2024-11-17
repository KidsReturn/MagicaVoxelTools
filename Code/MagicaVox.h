#pragma once

#include "CoreMinimal.h"
#include "VoxelAssets/VoxelDataAsset.h"

struct FVoxelDataAssetData;
struct ogt_vox_scene;
struct ogt_vox_model;
struct FVoxelIntBox;
using namespace UE::Math;

class IMagicaVoxelQueuedWork : public IQueuedWork
{
public:
	// Used for performance reporting and debugging
	const FName Name;

	IMagicaVoxelQueuedWork(FName Name) : Name(Name) {};

	IMagicaVoxelQueuedWork(const IMagicaVoxelQueuedWork&) = delete;
	IMagicaVoxelQueuedWork& operator=(const IMagicaVoxelQueuedWork&) = delete;
};

class FMagicaVoxelQueuedThreadPool : public TSharedFromThis<FMagicaVoxelQueuedThreadPool, ESPMode::ThreadSafe>
{
private:
	class FScopeLock
	{
	public:
		FScopeLock(FCriticalSection& InSyncObject)
			: SyncObject(InSyncObject)
		{
			SyncObject.Lock();
		}
		~FScopeLock()
		{
			SyncObject.Unlock();
		}

	private:
		FCriticalSection& SyncObject;
	};

	struct FQueuedWorkInfo
	{
		IMagicaVoxelQueuedWork* Work;

		FQueuedWorkInfo() = default;
		FQueuedWorkInfo(IMagicaVoxelQueuedWork* Work) : Work(Work) {};

		FORCEINLINE uint64 GetPriority() const
		{
			return 0;
		}
		FORCEINLINE bool operator<(const FQueuedWorkInfo& Other) const
		{
			return GetPriority() < Other.GetPriority();
		}
	};

	class FQueuedThread : public FRunnable
	{
	public:
		const FString ThreadName;
		FMagicaVoxelQueuedThreadPool* const ThreadPool;
		/** The event that tells the thread there is work to do. */
		FEvent* const DoWorkEvent;

		FQueuedThread(FMagicaVoxelQueuedThreadPool* Pool, const FString& ThreadName, uint32 StackSize, EThreadPriority ThreadPriority);
		~FQueuedThread();

		//~ Begin FRunnable Interface
		virtual uint32 Run() override;
		//~ End FRunnable Interface

	private:
		/** If true, the thread should exit. */
		FThreadSafeBool TimeToDie;

		const TUniquePtr<FRunnableThread> Thread;
	};

	explicit FMagicaVoxelQueuedThreadPool(uint32 NumThreads, uint32 StackSize, EThreadPriority ThreadPriority);

public:
	~FMagicaVoxelQueuedThreadPool();

	int32 GetNumThreads() const { return AllThreads.Num(); }
	bool IsWorking();

	void AddQueuedWork(IMagicaVoxelQueuedWork* InQueuedWork);
	void AddQueuedWorks(const TArray<IMagicaVoxelQueuedWork*>& InQueuedWorks);
	void AbandonAllTasks();

	IMagicaVoxelQueuedWork* ReturnToPoolOrGetNextJob(FQueuedThread* InQueuedThread);

	//
	static TSharedRef<FMagicaVoxelQueuedThreadPool, ESPMode::ThreadSafe> Create(int32 NumThreads, uint32 StackSize, EThreadPriority ThreadPriority);

private:
	TArray<TUniquePtr<FQueuedThread>> AllThreads;

	FCriticalSection Section;
	TArray<FQueuedThread*> QueuedThreads;

	std::priority_queue<FQueuedWorkInfo> QueuedWorks;

	FThreadSafeBool TimeToDie = false;
};

namespace MagicaVox
{
	bool ImportToAsset(const FString& Filename, FVoxelDataAssetData& Asset, const FVoxelDataAssetImportSettings_MagicaVox& InSetting);
	bool MergeSceneData(const ogt_vox_scene* InScene, TPair<FVoxelIntBox, TArray<uint8>>& OutData);
	bool UnifyModelData(const ogt_vox_model* InModel, const FMatrix44f& InMatrix, TPair<FVoxelIntBox, TArray<FUintVector4>>& OutData);

	class FMagicaVoxImportWork : public IMagicaVoxelQueuedWork
	{
	public:
		FMagicaVoxImportWork(FVoxelDataAssetData& InAssetData, const TArray<uint8>& InMagicaData, const FVoxelIntBox& InBounds, const FIntVector& InSceneSize, const FVoxelDataAssetImportSettings_MagicaVox& InSetting)
			: IMagicaVoxelQueuedWork("FMagicaVoxImportWork"), AssetData(InAssetData), MagicaData(InMagicaData), Bounds(InBounds), SceneSize(InSceneSize), Setting(InSetting) {};

		//~ Begin IQueuedWork Interface
		virtual void DoThreadedWork() override;
		virtual void Abandon() override;
		//~ End IQueuedWork Interface
		
		static TArray<IMagicaVoxelQueuedWork*> Create(FVoxelDataAssetData& InAssetData, const TPair<FVoxelIntBox, TArray<uint8>>& InSceneData, uint32 InNumThreads, const FVoxelDataAssetImportSettings_MagicaVox& InSetting);

	private:
		// shared code with @hexagon shader, check if they are synced while debugging.
		FVector GetCenter(const FVector& v, bool bDiagonal) const;
		int32 IsInbound(const FVector& c, const FVector& v) const;
		int32 GetBorderClockPos(FVector& c, const FVector& v) const;
		// shared code with @hexagon shader, check if they are synced while debugging.
		
		FVoxelDataAssetData& AssetData;
		const TArray<uint8>& MagicaData;
		const FVoxelIntBox Bounds;
		const FIntVector SceneSize;
		const FVoxelDataAssetImportSettings_MagicaVox Setting;
	};

	class FMagicaVoxMergeWork : public IMagicaVoxelQueuedWork
	{
	public:
		FMagicaVoxMergeWork(TArray<uint8>& InVoxelData, const FVoxelIntBox& InSceneBounds, const TPair<FVoxelIntBox, TArray<FUintVector4>>& InInstData) : IMagicaVoxelQueuedWork("FMagicaVoxMergeWork"), VoxelData(InVoxelData), InstData(InInstData), SceneBounds(InSceneBounds) {};

		//~ Begin IQueuedWork Interface
		virtual void DoThreadedWork() override;
		virtual void Abandon() override;
		//~ End IQueuedWork Interface

		static TArray<IMagicaVoxelQueuedWork*> Create(TArray<uint8>& InVoxelData, const FVoxelIntBox& InSceneBounds, const TMap<FVoxelIntBox, TArray<FUintVector4>>& InInstMap);

	private:
		TArray<uint8>& VoxelData;
		const TPair<FVoxelIntBox, TArray<FUintVector4>>& InstData;
		const FVoxelIntBox SceneBounds;
	};
}