from pathlib import Path
import math, shutil
root=Path('Resources/VR-Engine-Root/Valve-Index-Knuckles')
right=next(root.glob('**/*right.obj'))
old=right.read_bytes();lines=old.decode().splitlines();positions=[];normals=[];faces=[]
for line in lines:
 a=line.split()
 if not a:continue
 if a[0]=='v':positions.append(tuple(map(float,a[1:4])))
 if a[0]=='vn':normals.append(tuple(map(float,a[1:4])))
 if a[0]=='f':faces.append([s.split('/') for s in a[1:]])
sums=[[0.,0.,0.] for n in normals]
for face in faces:
 a,b,c=[positions[int(s[0])-1] for s in face[:3]]
 ab=[b[i]-a[i] for i in range(3)];ac=[c[i]-a[i] for i in range(3)]
 n=(ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0])
 for corner in face:
  idx=int(corner[2])-1
  for i in range(3):sums[idx][i]+=n[i]
fixed=[]
for oldn,n in zip(normals,sums):
 length=math.sqrt(sum(x*x for x in n));fixed.append(tuple(x/length for x in n) if length>1e-12 else oldn)
out=[];ni=0
for line in lines:
 if line.startswith('vn '):line='vn %.8f %.8f %.8f'%fixed[ni];ni+=1
 out.append(line)
new=('\n'.join(out)+'\n').encode()
project=Path(r'C:/Users/sansg/ModularityProjects/vr thingy/Assets/valve_controller_knu_1_0_right.obj')
if project.exists():
 assert project.read_bytes()==old,'Project OBJ differs; inspect before replacing'
 backup=project.parent.parent/'Library/LocomotionRegression/valve_controller_knu_1_0_right.before-normal-fix.obj'
 if not backup.exists():shutil.copy2(project,backup)
 project.write_bytes(new)
right.write_bytes(new)
print('Rebuilt',len(normals),'normals from',len(faces),'faces; preserved smoothing splits, positions, UVs and winding')
