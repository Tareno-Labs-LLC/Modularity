#include "Engine.h"
#include "XR/XRInput.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sstream>
namespace fs=std::filesystem;
void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
constexpr int W=640,H=480;
std::vector<float> Pixels(GLuint texture){std::vector<float> p(W*H*4);glBindTexture(GL_TEXTURE_2D,texture);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_FLOAT,p.data());return p;}
void Save(const fs::path& file,const std::vector<float>& p){std::ofstream f(file,std::ios::binary);f<<"P6\n"<<W<<" "<<H<<"\n255\n";for(int y=H-1;y>=0;--y)for(int x=0;x<W;++x)for(int c=0;c<3;++c){uint8_t b=uint8_t(std::clamp(p[(y*W+x)*4+c],0.f,1.f)*255);f.write(reinterpret_cast<char*>(&b),1);}}
int main(int argc,char**argv){try{
fs::path output=argc>1?argv[1]:".";fs::create_directories(output);Window window;auto*handle=window.makeWindow(Modularity::GraphicsBackend::OpenGL);Require(handle,"GL context");glfwHideWindow(handle);
{Renderer renderer;renderer.initialize();renderer.resize(W,H);renderer.setAmbientColor(glm::vec3(.6f));
Camera camera;camera.position={0,1.3f,4};camera.front=glm::normalize(glm::vec3(0,-.16f,-1));camera.solidBackground=true;
uint64_t serial=1;
auto render=[&](const std::vector<SceneObject>& scene,bool newFrame=true){renderer.setFrameSerial(newFrame?serial++:serial-1);auto p=BuildCameraProjection(camera,W,H,60,.1f,30);renderer.beginRender(camera.getViewMatrix(),p,camera.position);renderer.renderScene(camera,scene,-1,60,.1f,30,false,nullptr,SceneRenderMode::Normal,false);Require(glGetError()==GL_NO_ERROR,"Render GL error");return Pixels(renderer.getRawViewportTexture());};
auto cube=[](const char*name,int id,glm::vec3 pos,glm::vec3 scale){SceneObject o(name,ObjectType::Cube,id);o.position=pos;o.scale=scale;o.hasRenderer=true;o.renderType=RenderType::Cube;o.materialPath="fixture.mat";o.material.pbrEnabled=true;o.material.metallic=.8f;o.material.roughness=.08f;o.material.color=glm::vec3(.2f);return o;};
std::vector<SceneObject> scene;scene.push_back(cube("Floor",1,{0,-.1f,0},{10,.2f,12}));scene.push_back(cube("Wall",2,{0,2,-4},{10,4,.2f}));
SceneObject canvas("Canvas",ObjectType::Canvas,3);canvas.hasRenderer=true;canvas.renderType=RenderType::Plane;canvas.hasUI=true;canvas.ui.type=UIElementType::Canvas;canvas.ui.renderIn3D=true;canvas.position={0,.9f,0};canvas.scale={1.4f,1.4f,1};canvas.materialPath="fixture.mat";scene.push_back(canvas);
auto sky=renderer.getSkybox()->getSettings();sky.environmentReflections=false;sky.ssrIntensity=1;sky.ssrSteps=160;sky.ssrRefineSteps=12;sky.ssrMaxDistance=20;sky.ssrThickness=.15f;sky.ssrHalfResolution=false;
for(bool atlas:{false,true}){
auto info=renderer.ensureUiTarget(3,64,64,0,MaterialProperties::TextureFilter::Bilinear,atlas);
std::vector<uint8_t> pixels(64*64*4);for(int y=0;y<64;++y)for(int x=0;x<64;++x){size_t i=(y*64+x)*4;pixels[i]=255;pixels[i+1]=12;pixels[i+2]=3;pixels[i+3]=(x>7&&x<56&&y>7&&y<56)?255:0;}
// Atlas slots use top-left canvas coordinates; GL texture rows start at the bottom.
const int textureY=info.framebufferHeight-info.y-info.height;
glBindTexture(GL_TEXTURE_2D,info.texture);glTexSubImage2D(GL_TEXTURE_2D,0,info.x,textureY,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());renderer.markUiTargetRendered(3);
sky.ssrEnabled=false;renderer.getSkybox()->setSettings(sky);auto baseline=render(scene);
sky.ssrEnabled=true;renderer.getSkybox()->setSettings(sky);auto reflected=render(scene);
std::vector<float> depth(W*H);glBindTexture(GL_TEXTURE_2D,renderer.getSceneDepthTexture());glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT,GL_FLOAT,depth.data());
auto project=[&](glm::vec3 world){glm::vec4 c=BuildCameraProjection(camera,W,H,60,.1f,30)*camera.getViewMatrix()*glm::vec4(world,1);return glm::vec3(c)/c.w*.5f+.5f;};
auto center=project(canvas.position);int cx=int(center.x*W),cy=int(center.y*H);Require(std::abs(depth[cy*W+cx]-center.z)<.00001f,"Canvas has no world-position hit depth");
auto corner=project(canvas.position+glm::vec3(-.64f,.64f,0));int hx=int(corner.x*W),hy=int(corner.y*H);Require(depth[hy*W+hx]>corner.z+.0001f,"Transparent UI corner overwrote background depth");
// The panel itself must not receive floor normals/reflections. Its reflection
// belongs on floor pixels below it, with hits positioned at the panel's depth.
double reflection=0;int hits=0;const auto lower=project(canvas.position+glm::vec3(0,-.7f,0));
for(int y=0;y<int(lower.y*H)-2;++y)for(int x=W/4;x<W*3/4;++x){size_t i=(y*W+x)*4;float increase=reflected[i]-baseline[i];if(increase>.003f&&increase>(reflected[i+1]-baseline[i+1])*2){reflection+=increase;++hits;}}
std::cout<<"UI floor reflection atlas="<<atlas<<" hits="<<hits<<" energy="<<reflection<<'\n';Require(hits>10,"UI reflection is not on the floor");
Save(output/(atlas?"ui-atlas.ppm":"ui-standalone.ppm"),reflected);
Require(glGetError()==GL_NO_ERROR,"UI surface GL error");
}
std::cout<<"PASS: UI hit depth, transparent corners and floor reflections for standalone and atlas targets\n";
// Real renderer, simulated tracking; no headset/active OpenXR runtime required.
std::vector<SceneObject> rays;SceneObject controller("Controller",ObjectType::Empty,20);controller.hasXRActionBasedController=true;controller.hasXRRayInteractor=true;controller.position={-.35f,.9f,2};controller.rotation={0,-20,0};controller.xrRayInteractor.lineColor={.1f,1,.2f,1};controller.xrRayInteractor.hasHit=true;controller.xrRayInteractor.hitPoint=controller.position+EulerXYZDegreesToQuat(controller.rotation)*glm::vec3(0,0,-1.5f);rays.push_back(controller);
auto& input=Modularity::XR::MutableInputState();input.sessionActive=true;input.controllers[0].active=true;input.controllers[0].aimPose.positionTracked=true;input.controllers[0].aimPose.orientationTracked=true;
auto beam=render(rays);int green=0;for(size_t i=0;i<beam.size();i+=4)if(beam[i+1]>.5f&&beam[i+1]>beam[i]*2)++green;Require(green>10,"Controller beam missing");Save(output/"controller-ray.ppm",beam);
rays[0].xrRayInteractor.showLineVisual=false;auto hidden=render(rays);int hiddenGreen=0;for(size_t i=0;i<hidden.size();i+=4)if(hidden[i+1]>.5f&&hidden[i+1]>hidden[i]*2)++hiddenGreen;Require(hiddenGreen==0,"Disabled ray still renders");
rays[0].xrRayInteractor.showLineVisual=true;input.controllers[0].active=false;auto untracked=render(rays);Require(untracked==hidden,"Untracked controller leaves a stale ray");Modularity::XR::ResetInputState();
std::cout<<"PASS: visible controller beam/reticle, showLineVisual toggle and tracking-loss hiding\n";
sky.ssrEnabled=false;renderer.getSkybox()->setSettings(sky);
std::vector<SceneObject> casters{cube("Caster",40,{0,1,0},{1,1,1})};
SceneObject point("Point",ObjectType::PointLight,41);point.hasLight=true;point.light.type=LightType::Point;point.light.castShadows=true;point.light.range=10;point.position={0,3,2};casters.push_back(point);
renderer.setShadowMapResolution(128);render(casters);Require(renderer.getLastViewportStats().shadowMapUpdates==1,"Point shadow was not rendered");
camera.position.x+=.03f;render(casters,false);Require(renderer.getLastViewportStats().shadowMapReuses==1,"Point shadow was not shared across views");
SceneWindState changedWind;changedWind.time=1;renderer.setSceneWind(changedWind);render(casters,false);Require(renderer.getLastViewportStats().shadowMapUpdates==1,"Changed wind reused stale shadow geometry");
render(casters);Require(renderer.getLastViewportStats().shadowMapUpdates==1,"Next frame reused stale shadows");
casters[0].faceCamera=true;render(casters,false);Require(renderer.getLastViewportStats().shadowMapUpdates==1,"View-dependent geometry reused another view's shadows");
std::cout<<"PASS: point shadows share within one frame and refresh for new frames, wind and view-dependent geometry\n";
// Hot reload still observes file edits after timestamp I/O moves off the GL thread.
const auto vertex=fs::absolute(output/"reload.vert"), fragment=fs::absolute(output/"reload.frag");
{std::ofstream v(vertex);v<<"#version 330 core\nlayout(location=0) in vec3 p;void main(){gl_Position=vec4(p,1);}";}
const auto writeFragment=[&](const char* color){std::ofstream f(fragment);f<<"#version 330 core\nout vec4 c;void main(){c=vec4("<<color<<");}";};
writeFragment("1,0,0,1");auto* initial=renderer.getShader(vertex.string(),fragment.string());Require(initial&&initial->ID,"Reload fixture compile");
const GLuint initialId=initial->ID;const auto oldTime=fs::last_write_time(fragment);
writeFragment("0,1,0,1");fs::last_write_time(fragment,oldTime+std::chrono::seconds(2));
renderer.setShaderAutoReload(false);Require(renderer.getShader(vertex.string(),fragment.string())->ID==initialId,"Disabled reload replaced shader");
renderer.setShaderAutoReload(true);
const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);bool reloaded=false;
while(std::chrono::steady_clock::now()<deadline){if(renderer.getShader(vertex.string(),fragment.string())->ID!=initialId){reloaded=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
Require(reloaded,"Background file watcher missed shader edit");
Require(renderer.forceReloadShader(vertex.string(),fragment.string()),"Forced shader reload failed");
const GLuint forced=renderer.getShader(vertex.string(),fragment.string())->ID;
const auto stableDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1100);
while(std::chrono::steady_clock::now()<stableDeadline){Require(renderer.getShader(vertex.string(),fragment.string())->ID==forced,"Old background timestamp retriggered forced reload");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
std::cout<<"PASS: background shader timestamp polling, reload toggle and forced reload stability\n";
const auto badTexture=fs::absolute(output/"retry-texture.png");
{std::ofstream f(badTexture,std::ios::binary);f<<"unsupported image data";}
const auto badTime=fs::last_write_time(badTexture);
Require(renderer.getTexture(badTexture.string(),MaterialProperties::TextureFilter::Bilinear)==nullptr,"Bad texture unexpectedly decoded");
// Cross the old 30-second deadline without slowing down this regression check.
const double clockBefore=glfwGetTime();glfwSetTime(clockBefore+31);
std::ostringstream retriedErrors;auto* oldErrors=std::cerr.rdbuf(retriedErrors.rdbuf());
auto* unchanged=renderer.getTexture(badTexture.string(),MaterialProperties::TextureFilter::Bilinear);
std::cerr.rdbuf(oldErrors);glfwSetTime(clockBefore);
Require(!unchanged&&retriedErrors.str().empty(),"Unchanged bad texture was decoded again after 30 seconds");
{std::ofstream f(badTexture,std::ios::binary);f<<"P6\n1 1\n255\n";const char rgb[3]={0,127,0};f.write(rgb,3);}
fs::last_write_time(badTexture,badTime+std::chrono::seconds(2));
Texture* recovered=nullptr;const auto recoveryDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
while(std::chrono::steady_clock::now()<recoveryDeadline){recovered=renderer.getTexture(badTexture.string(),MaterialProperties::TextureFilter::Bilinear);if(recovered)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
Require(recovered&&recovered->GetID(),"Edited failed texture was not retried");
std::cout<<"PASS: unchanged decode failures stay cached past 30 seconds; edited textures recover\n";
}
Require(glGetError()==GL_NO_ERROR,"Cleanup GL error");std::cout<<"PASS: tree/UI renderer integration\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
