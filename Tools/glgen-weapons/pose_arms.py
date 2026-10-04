"""Bake para/MakeHuman CC0 arm rig into AK-specific gripping poses.
Run with Blender --background --python pose_arms.py -- <source.blend> <outdir>.
Only the two small arm meshes are used at runtime, not the source scene's lights.
"""
import bpy,sys,math,json,struct,shutil
from pathlib import Path
from mathutils import Vector,Matrix
args=sys.argv[sys.argv.index('--')+1:]
bpy.ops.wm.open_mainfile(filepath=args[0]);out=Path(args[1]);out.mkdir(parents=True,exist_ok=True)
scene=bpy.context.scene;scene.frame_set(0)
rig=next(o for o in bpy.data.objects if o.type=='ARMATURE')
mesh=next(o for o in bpy.data.objects if o.type=='MESH')
rig.animation_data_clear()
# Source proportions are ten times larger than metre units.
rig.scale=Vector((.1,.1,.1));rig.location=Vector((0,-.32,-.20))
bpy.context.view_layer.update()
def world_to_engine(p):return Vector((p.x,p.z,-p.y))
def engine_to_world(p):return Vector((p[0],-p[2],p[1]))
pivots={'R':(.032,-.065,.035),'L':(-.032,.025,-.30)}
for side in ['R','L']:
 control=rig.pose.bones['hand.'+side+'.control']
 hand=rig.pose.bones['hand.'+side]
 middle=rig.pose.bones['palm_middle.'+side]
 source=(rig.matrix_world@middle.head)-(rig.matrix_world@hand.head)
 target=engine_to_world((-.025,.055,-.025) if side=='R' else (.05,.035,-.025))
 rotation=source.normalized().rotation_difference(target.normalized())
 world=rig.matrix_world@control.matrix
 rot=(rotation@world.to_quaternion()).to_matrix().to_4x4()
 rot=rot@Matrix.Diagonal((*world.to_scale(),1.0))
 rot.translation=engine_to_world(pivots[side])
 # Pose-bone matrix is in armature space; the author IK solves the elbows.
 control.matrix=rig.matrix_world.inverted()@rot
 bpy.context.view_layer.update()
 for finger in ['pinky','middle','ring','index']:
  b=rig.pose.bones['f_'+finger+'.01.'+side]
  b.rotation_mode='YZX'
  b.rotation_euler.x=math.radians(62 if finger!='index' or side=='L' else 18)
 # Thumb opposition retains the source's hand roll, with a small curl.
 rig.pose.bones['thumb.02.'+side].rotation_euler.x=math.radians(35)
 bpy.context.view_layer.update()
 thumb=rig.pose.bones['thumb.01.'+side]
 head=rig.matrix_world@thumb.head
 endpoint=engine_to_world((.018,.095,-.34) if side=='L' else (-.012,.045,-.025))
 direction=(rig.matrix_world@thumb.tail)-head
 q=direction.normalized().rotation_difference((endpoint-head).normalized())
 world=rig.matrix_world@thumb.matrix
 matrix=(q@world.to_quaternion()).to_matrix().to_4x4();matrix=matrix@Matrix.Diagonal((*world.to_scale(),1.0));matrix.translation=head
 thumb.matrix=rig.matrix_world.inverted()@matrix
 bpy.context.view_layer.update()
deps=bpy.context.evaluated_depsgraph_get();obj=mesh.evaluated_get(deps);m=obj.to_mesh();m.calc_loop_triangles()
groups={i.name:i.index for i in mesh.vertex_groups}
left_groups={i.index for i in mesh.vertex_groups if '.L' in i.name}
side_for_vertex=[]
for v in mesh.data.vertices:
 lw=sum(g.weight for g in v.groups if g.group in left_groups)
 tw=sum(g.weight for g in v.groups)
 side_for_vertex.append('L' if lw>tw*.5 else 'R')
parts={side:{'p':[],'n':[],'uv':[]} for side in ['L','R']}
uv=m.uv_layers.active
normalmat=obj.matrix_world.to_3x3().inverted().transposed()
for tri in m.loop_triangles:
 side='L' if sum(side_for_vertex[v]=='L' for v in tri.vertices)>=2 else 'R'
 part=parts[side]
 for loop in tri.loops:
  p=world_to_engine(obj.matrix_world@m.vertices[m.loops[loop].vertex_index].co)-Vector(pivots[side])
  n=world_to_engine((normalmat@m.corner_normals[loop].vector).normalized())
  part['p']+=list(p);part['n']+=list(n);part['uv']+=list(uv.data[loop].uv)[:1]+[1-uv.data[loop].uv.y]
for side,part in parts.items():
 name='hand_left' if side=='L' else 'hand_right'
 raw=bytearray();views=[];access=[]
 for key,size,kind in [('p',3,'VEC3'),('n',3,'VEC3'),('uv',2,'VEC2')]:
  values=part[key];views.append({'buffer':0,'byteOffset':len(raw),'byteLength':len(values)*4,'target':34962})
  raw+=struct.pack('<'+'f'*len(values),*values)
  a={'bufferView':len(views)-1,'componentType':5126,'count':len(values)//size,'type':kind}
  if key=='p':
   a['min']=[min(values[i::3]) for i in range(3)];a['max']=[max(values[i::3]) for i in range(3)]
  access.append(a)
 gltf={'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'name':name,'mesh':0}],
  'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'material':0}]}],
  'buffers':[{'uri':name+'.bin','byteLength':len(raw)}],'bufferViews':views,'accessors':access,
  'materials':[{'name':'Photographic skin','pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0,'roughnessFactor':.57}}],
  'images':[{'uri':'hands_albedo.png'}],'textures':[{'source':0}]}
 (out/(name+'.bin')).write_bytes(raw);(out/(name+'.gltf')).write_text(json.dumps(gltf,indent=2))
 print('EXPORTED',name,len(part['p'])//9,'bounds',access[0]['min'],access[0]['max'],'wrist',pivots[side])
rigfile=out/'rig.json';metadata=json.loads(rigfile.read_text());metadata['parts']=[p for p in metadata['parts'] if not p['name'].startswith('hand_')]
for side in ['L','R']:
 name='hand_left' if side=='L' else 'hand_right';metadata['parts'].append({'name':name,'file':name+'.gltf','pivot':pivots[side],'triangles':len(parts[side]['p'])//9})
rigfile.write_text(json.dumps(metadata,indent=2));shutil.copy2(Path(args[0]).parent/'new_diff.png',out/'hands_albedo.png')
obj.to_mesh_clear()
