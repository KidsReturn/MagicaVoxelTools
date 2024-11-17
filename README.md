# **Problem**
Currently I'm developing a SLG game which ideally has a voxel based hexagon tile map(yea, it's a weired combination).

There are two tools fits this need:
1) MagicaVoxel to generate the map.
2) VoxelPlugin to import it and render in Unreal Engine.

But while testing them out, few obstacles I bump into:
1) MagicaVoxel has a 256^3 limit per object, and VoxelPlugin can only import one object at a time.
2) The Hexagons have ugly edges since they are approximated by cubes.![屏幕截图 2024-11-17 233956](https://github.com/user-attachments/assets/c6f8a9f6-b4d1-457c-9050-a87985f3d2cb)


# **Solution**
A shader to procedural generate infinite hexagon tile. 

And a importer to bypass the 256^3 limit and smooth out the edge surface.

## **Result**
![屏幕截图 2024-11-17 225410](https://github.com/user-attachments/assets/660176b2-53fe-43c8-a6a8-0288ae00ae32)

## **Shader**
Simply put under MagicaVoxel's shader folder and it will be available on next boot up.

### **Arguments:**

Mode:

0: fill volume with current color index and AltColor on border
![屏幕截图 2024-11-17 230424](https://github.com/user-attachments/assets/c9deed42-af53-4643-ae33-9ee35da8affd)

few click with face tool, you will be able to create hexagon-based terrain:
![屏幕截图 2024-11-17 225319](https://github.com/user-attachments/assets/f66573a2-8da4-4e72-8657-ecfe719dee4a)

1: expand border with selected color(just click execute shader!)
![屏幕截图 2024-11-17 230440](https://github.com/user-attachments/assets/29aafe8b-a178-4cc3-b484-14b627aee650)

2: cull border(just click execute shader!)

3: fill volume with AltColor and current color index on border

4 - only generate one hexagon

AltColor: index color to fill blanks/border
	
HalfWidth: half width of the hexagon
	
Rotation: which axis to face

## **Code**
The code is a multi-thread importer for MagicaVoxel which mainly do two things: 

1. allow user to create large scale scene consist of multiple objects to bypass the 256x256x256 limit in MagicaVoxel, and then import them all together into UnrealEngine as one object.

	Seems quite easy, but the trick part is how the transform is handled. 
	
	Couple things i noticed: 
	1. Voxel's position value is one of eight vertex's position base on current rotation.
	2. When object is rotated, the object's pivot is also rotated along. Therefore the position value in exported file is not the same as the value shown in editor.
	3. Voxel data in exported file is arranged in local space, not rearranged with transform while exporting.

2. calculate proper voxel value so that marching cube can form a smooth mesh surface.
	1. it reuse some algrithms in the shader above
