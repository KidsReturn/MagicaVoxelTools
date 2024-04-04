# **Few tools i made while using MagicaVoxel**

## **Code**

### **Merge scene objects**

Goal of these code is to create a work flow like this: 

1. create large scale scene consist of multiple objects to bypass the 256x256x256 limit in MagicaVoxel
2.  then import them all together into UnrealEngine as one object.

Seems quite easy, but the trick part is how the transform is handled. 

Couple things i noticed: 
1. Voxel's position value is one of eight vertex's position base on current rotation.
2. When object is rotated, the object's pivot is also rotated along. Therefore the position value in exported file is not the same as the value shown in editor.
3. Voxel data in exported file is arranged in local space, not rearranged with transform while exporting.

Here is two functions that address these issues while using ogt_vox and voxel plugin.
```
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
	for (const TPair<FVoxelIntBox, TArray<FUintVector4>>& Inst : InstMap)
	{
		const FVoxelIntBox& Bounds = Inst.Key;
		FIntVector Origin = Bounds.Min - SceneBounds.Min;
		FIntVector SceneSize = SceneBounds.Size();
		for (const FUintVector4& Data : Inst.Value)
		{
			VoxelData[Origin.X + Data.X + SceneSize.X * (Origin.Y + Data.Y) + SceneSize.X * SceneSize.Y * (Origin.Z + Data.Z)] = Data.W;
		}
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
```

## **Shader**
Simply put under MagicaVoxel's shader folder and it will be available on next boot up.

### **Hexagon Generator**

You can easily generate hexagon tile using shader tool like this:

![image](https://github.com/KidsReturn/MagicaVoxelShader/assets/41110770/633b5d46-eea7-4e50-8bda-6ac2b4580aeb)

few click with face tool, you will be able to create hexagon-based terrain:

![image](https://github.com/KidsReturn/MagicaVoxelShader/assets/41110770/4dcd5e8e-c541-4c3e-af35-6b445036aa18)

### **Arguments:**

  Mode: 
  
   0 - fill volume using AltColor on border, 
   ![屏幕截图 2024-03-31 145205](https://github.com/KidsReturn/MagicaVoxelShader/assets/41110770/9ac3299d-137d-4413-a142-8dba6e468e4c)
  1 - fill volume using AltColor on inside, 
   ![屏幕截图 2024-03-31 145225](https://github.com/KidsReturn/MagicaVoxelShader/assets/41110770/ec332c86-ca9d-402c-bba5-ad2424cc3d94)
   2 - only generate one hexagon
    ![屏幕截图 2024-03-31 145118](https://github.com/KidsReturn/MagicaVoxelShader/assets/41110770/de6dad66-9527-4512-8ee3-ee21b26e6fb8)
	
  AltColor: index color to fill blanks/border
	
  Radius: size of the hexagon
	
  Rotation: which axis to face
