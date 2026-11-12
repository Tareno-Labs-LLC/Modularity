"""Depth AO and camera-motion blur regression on the real OpenGL driver.
Run: python tools/shader-selftest/postfx_test.py [--benchmark]
"""
from pathlib import Path
import sys
import numpy as np
import moderngl

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/postfx-selftest'
OUT.mkdir(parents=True, exist_ok=True)
ctx = moderngl.create_standalone_context(require=330)
print(ctx.info['GL_RENDERER'])
parking=ctx.framebuffer([ctx.texture((1,1),4)])
parking.use()
vertex = (ROOT / 'Resources/Shaders/postfx_vert.glsl').read_text()
programs = {name: ctx.program(vertex_shader=vertex, fragment_shader=(ROOT / 'Resources/Shaders' / name).read_text())
            for name in ['postfx_frag.glsl', 'postfx_ao_frag.glsl', 'postfx_ao_blur_frag.glsl']}
vbo = ctx.buffer(np.array([-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1], dtype='f4').tobytes())
vaos = {name: ctx.vertex_array(p, [(vbo, '2f 2f', 'aPos', 'aTexCoord')]) for name,p in programs.items()}

def check(label):
    error=ctx.error
    assert error=='GL_NO_ERROR', label+': '+error

def texture(data, linear=False):
    if data.ndim == 2: data = data[...,None]
    h,w,c = data.shape
    tex = ctx.texture((w,h), c, np.ascontiguousarray(data, dtype='f4').tobytes(), dtype='f4')
    tex.filter = (moderngl.LINEAR, moderngl.LINEAR) if linear else (moderngl.NEAREST, moderngl.NEAREST)
    tex.repeat_x = tex.repeat_y = False
    return tex

def uniform(p, **values):
    for name,value in values.items():
        if name not in p: continue
        if isinstance(value,np.ndarray): p[name].write(value.T.astype('f4').tobytes())
        else: p[name].value = value

def draw(name, target, size=None):
    target.use();ctx.viewport = (0,0,*(size or target.size))
    vaos[name].render(moderngl.TRIANGLE_STRIP)

def read(fbo):
    return np.frombuffer(fbo.read(components=4,dtype='f4'),dtype='f4').reshape(fbo.height,fbo.width,4)

def projection(w,h, offset=0, ortho=False):
    n,f = .1,100.
    if ortho:
        return np.array([[.35*h/w,0,0,offset],[0,.35,0,0],[0,0,-2/(f-n),-(f+n)/(f-n)],[0,0,0,1]],dtype='f4')
    y = 1/np.tan(np.radians(60)/2)
    return np.array([[y*h/w,0,offset,0],[0,y,0,0],[0,0,-(f+n)/(f-n),-2*f*n/(f-n)],[0,0,-1,0]],dtype='f4')

def scene(w,h,p,view=None):
    if view is not None: p=p@view
    x,y = np.meshgrid((np.arange(w)+.5)/w*2-1,(np.arange(h)+.5)/h*2-1)
    ndc = np.stack([x,y,-np.ones_like(x),np.ones_like(x)],axis=-1)
    origin = ndc @ np.linalg.inv(p).T
    origin = origin[...,:3]/origin[...,3:]
    ndc[...,2] = 1
    end = ndc @ np.linalg.inv(p).T; end=end[...,:3]/end[...,3:]
    ray = end-origin
    ray /= np.linalg.norm(ray,axis=-1,keepdims=True)
    floor_t = np.divide(-1-origin[...,1], ray[...,1], out=np.full((h,w),np.inf), where=ray[...,1]<-1e-6)
    wall_t = (-6-origin[...,2])/ray[...,2]
    with np.errstate(divide='ignore', invalid='ignore'):
        entry = (np.array([-.7,-1,-4])-origin)/ray
        leave = (np.array([.7,.4,-3])-origin)/ray
    box_enter = np.minimum(entry,leave).max(axis=-1)
    box_leave = np.maximum(entry,leave).min(axis=-1)
    box_t = np.where((box_enter>0)&(box_enter<box_leave),box_enter,np.inf)
    distance = np.minimum(np.minimum(floor_t,wall_t),box_t)
    points = origin + ray*distance[...,None]
    clip = np.concatenate([points,np.ones((h,w,1))],axis=-1) @ p.T
    depth = (clip[...,2]/clip[...,3]*.5+.5).astype('f4')
    return depth, points, (box_t == distance)

def ao(depth,p,radius=.65,padded=False):
    h,w = depth.shape; size=((w+1)//2,(h+1)//2)
    check('ao entry')
    depth_tex = texture(depth)
    check('depth create')
    storage=(size[0]*2,size[1]*2) if padded else size
    a=ctx.texture(storage,2,dtype='f2'); b=ctx.texture(storage,2,dtype='f2')
    for t in [a,b]: t.filter=(moderngl.LINEAR,moderngl.LINEAR); t.repeat_x=t.repeat_y=False
    fa=ctx.framebuffer([a]);fb=ctx.framebuffer([b]);check('ao buffers')
    uniform(programs['postfx_ao_frag.glsl'],sceneDepth=0,inverseProjection=np.linalg.inv(p),projection=p,depthTexelSize=(1/w,1/h),radius=radius)
    depth_tex.use(0);draw('postfx_ao_frag.glsl',fa,size);check('ao draw')
    uniform(programs['postfx_ao_blur_frag.glsl'],sourceTex=0,radius=radius,sourceScale=(size[0]/storage[0],size[1]/storage[1]),sourceTexelSize=(1/storage[0],1/storage[1]))
    for source,target,step in [(a,fb,(1/storage[0],0)),(b,fa,(0,1/storage[1]))]:
        source.use(0);uniform(programs['postfx_ao_blur_frag.glsl'],stepUv=step);draw('postfx_ao_blur_frag.glsl',target,size)
    # Exercise the exact joint upsample in the shipping composite.
    image=texture(np.ones((h,w,3),dtype='f4')); result=ctx.framebuffer([ctx.texture((w,h),4,dtype='f4')])
    post=programs['postfx_frag.glsl']
    uniform(post, sceneTex=0,bloomTex=1,sceneDepth=2,aoTex=3,enableHDR=False,gamma=1.,enableAO=True,aoActiveSize=size,aoRadius=radius,aoStrength=1.,enableMotionBlur=False,enableColorAdjust=False,texelSize=(1/w,1/h))
    uniform(post,inverseProjection=np.linalg.inv(p))
    image.use(0);depth_tex.use(2);a.use(3);draw('postfx_frag.glsl',result)
    check('ao composite')
    pixels=read(result)
    error=ctx.error
    assert error=='GL_NO_ERROR', 'AO: '+error
    parking.use()
    for t in [depth_tex,a,b,fa,fb,image,*result.color_attachments,result]: t.release()
    check('ao cleanup')
    assert np.isfinite(pixels).all()
    return pixels[...,0]

w,h=320,240
p=projection(w,h)
depth,points,box=scene(w,h,p)
a=ao(depth,p)
small=ao(depth,p,.2)
assert a.min()<.85, f'Missing contact occlusion: {a.min()}'
assert a.mean()<small.mean(), 'World radius should increase contact reach'
# Flat untextured geometry has no occlusion. The old color-contrast AO cannot pass this.
flat=np.full((h,w),.98,dtype='f4')
flat_ao=ao(flat,p)

assert np.max(np.abs(flat_ao-1))<.001, 'Self-occlusion on a flat plane'
sky=np.ones((h,w),dtype='f4')
assert np.min(ao(sky,p))>.999, 'AO must not darken the sky'
ortho=projection(w,h,ortho=True)
angle=np.radians(30)
view=np.array([[1,0,0,0],[0,np.cos(angle),-np.sin(angle),0],[0,np.sin(angle),np.cos(angle),0],[0,0,0,1]],dtype='f4')
ortho_depth,_,_=scene(w,h,ortho,view)
assert ao(ortho_depth,ortho).min()<.9, 'Orthographic contacts'
for offset in [-.12,.12]:
    eye=projection(w,h,offset); eye_depth,_,_=scene(w,h,eye)
    eye_ao=ao(eye_depth,eye)
    assert eye_ao.min()<.9, 'Off-axis eye missing AO'
odd_p=projection(317,239)
odd_depth,_,_=scene(317,239,odd_p)
assert np.max(np.abs(ao(odd_depth,odd_p)-ao(odd_depth,odd_p,padded=True)))<.0001, 'Reusing larger AO storage changes output'
print('PASS: odd resolution and bounded reuse of larger AO buffers')
print(f'PASS: AO contacts/radius/flat plane/sky/orthographic/off-axis eyes; contact min={a.min():.3f}')
# PPM for inspectable deterministic evidence.
preview=np.repeat(a[...,None],3,axis=-1)
(OUT/'ao-contacts.ppm').write_bytes(f'P6\n{w} {h}\n255\n'.encode()+np.uint8(np.clip(preview[::-1],0,1)*255).tobytes())

def blur(color,depth,reprojection,strength=1,history=True,clamp=.6,threshold=0,shutter=1):
    h,w=depth.shape
    image=texture(color,linear=True);d=texture(depth,linear=True)
    target=ctx.framebuffer([ctx.texture((w,h),4,dtype='f4')])
    post=programs['postfx_frag.glsl']
    uniform(post,sceneTex=0,sceneDepth=2,enableAO=False,enableMotionBlur=True,hasHistory=history,motionBlurStrength=strength,motionBlurClamp=clamp,motionBlurThreshold=threshold,motionShutterScale=shutter,currentToPreviousClip=reprojection,inverseProjection=np.linalg.inv(projection(w,h)),enableHDR=False,gamma=1.,texelSize=(1/w,1/h))
    image.use(0);d.use(2);draw('postfx_frag.glsl',target)
    pixels=read(target)[...,:3]
    error=ctx.error
    assert error=='GL_NO_ERROR', 'Blur: '+error
    parking.use()
    for t in [image,d,*target.color_attachments,target]:t.release()
    assert np.isfinite(pixels).all()
    return pixels

x=np.arange(w);stripes=np.broadcast_to(((x//3)%2)[None,:,None],(h,w,3)).astype('f4').copy()
identity=np.eye(4,dtype='f4');moving=identity.copy();moving[0,3]=.12
still=blur(stripes,flat,identity)
assert np.max(np.abs(still-stripes))<1e-4, 'Stationary camera should stay sharp'
assert np.max(np.abs(blur(stripes,flat,moving,history=False)-stripes))<1e-4, 'First frame must stay sharp'
assert np.max(np.abs(blur(stripes,flat,moving,clamp=0)-stripes))<1e-4, 'Zero blur clamp'
fast=blur(stripes,flat,moving)
short=identity.copy();short[0,3]=1.5/w
assert np.mean(np.abs(blur(stripes,flat,short)-stripes))>.005, 'Subpixel blur incorrectly snaps back to center'
assert fast[:,20:-20].std()<still[:,20:-20].std()*.65, 'Camera motion is not blurring'
clamped=blur(stripes,flat,moving,clamp=.01)
assert clamped[:,20:-20].std()>fast[:,20:-20].std(), 'Maximum blur does not limit the trail'
frame90=moving.copy();frame90[0,3]*=2/3
assert np.max(np.abs(blur(stripes,flat,frame90,shutter=1.5)-fast))<1e-4, 'Exposure differs between 60 and 90 Hz'
assert np.max(np.abs(blur(stripes,flat,moving,threshold=5)-stripes))<1e-4, 'Minimum motion threshold'
# A red foreground against blue background must not produce purple edge halos.
colors=np.zeros((h,w,3),dtype='f4');colors[:,:w//2,0]=1;colors[:,w//2:,2]=1
split=flat.copy();split[:,:w//2]=.94
edges=blur(colors,split,moving)
assert edges[:,:w//2-2,2].max()<.001 and edges[:,w//2+2:,0].max()<.001, 'Blur leaks through depth silhouettes'
print('PASS: motion reprojection/subpixel/first frame/stationary/threshold/clamp/depth rejection/60-90 Hz exposure')
(OUT/'motion-blur.ppm').write_bytes(f'P6\n{w} {h}\n255\n'.encode()+np.uint8(np.clip(fast[::-1],0,1)*255).tobytes())
error=ctx.error
assert error=='GL_NO_ERROR',error
print('PASS: all post-FX shaders compile/link and run without GL errors')

if '--benchmark' in sys.argv:
    from statistics import median
    w,h=1920,1080
    p=projection(w,h)
    # Reuse a depth fixture at display resolution; allocations and readbacks are
    # outside the timed region. Query reads are intentionally blocking only here.
    d=np.full((h,w),.98,dtype='f4');d[h//3:2*h//3,w//3:2*w//3]=.96
    depth_tex=texture(d,linear=True)
    image=texture(np.broadcast_to(np.linspace(0,1,w,dtype='f4')[None,:,None],(h,w,3)).copy(),True)
    a=ctx.texture((w//2,h//2),2,dtype='f2');b=ctx.texture((w//2,h//2),2,dtype='f2')
    for t in [a,b]:t.filter=(moderngl.LINEAR,moderngl.LINEAR);t.repeat_x=t.repeat_y=False
    fa=ctx.framebuffer([a]);fb=ctx.framebuffer([b]);target=ctx.framebuffer([ctx.texture((w,h),4,dtype='f2')])
    uniform(programs['postfx_ao_frag.glsl'],sceneDepth=0,inverseProjection=np.linalg.inv(p),projection=p,depthTexelSize=(1/w,1/h),radius=.65)
    uniform(programs['postfx_ao_blur_frag.glsl'],sourceTex=0,radius=.65,sourceScale=(1.,1.),sourceTexelSize=(2/w,2/h))
    post=programs['postfx_frag.glsl']
    uniform(post,sceneTex=0,bloomTex=1,sceneDepth=2,aoTex=3,enableHDR=False,gamma=1.,inverseProjection=np.linalg.inv(p),texelSize=(1/w,1/h),aoActiveSize=(w//2,h//2),aoRadius=.65,aoStrength=1.,hasHistory=True,motionBlurStrength=1.,motionShutterScale=1.,motionBlurClamp=.35,motionBlurThreshold=0.,currentToPreviousClip=moving)
    def frame(ao_on,mb_on):
        if ao_on:
            depth_tex.use(0);draw('postfx_ao_frag.glsl',fa)
            a.use(0);uniform(programs['postfx_ao_blur_frag.glsl'],stepUv=(2/w,0));draw('postfx_ao_blur_frag.glsl',fb)
            b.use(0);uniform(programs['postfx_ao_blur_frag.glsl'],stepUv=(0,2/h));draw('postfx_ao_blur_frag.glsl',fa)
        uniform(post,enableAO=ao_on,enableMotionBlur=mb_on)
        image.use(0);depth_tex.use(2);a.use(3);draw('postfx_frag.glsl',target)
    for kind in [(False,False),(True,False),(False,True),(True,True)]:
        for _ in range(5):frame(*kind)
        times=[]
        for _ in range(9):
            query=ctx.query(time=True)
            with query:
                for i in range(4):frame(*kind)
            times.append(query.elapsed/4e6)
        print(f'GPU 1920x1080 AO={kind[0]} motion={kind[1]}: {median(times):.3f} ms (median, includes composite)')
    check('benchmark')
