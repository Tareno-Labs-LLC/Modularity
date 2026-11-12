#include "Engine.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
namespace fs = std::filesystem;
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void NoError(const char* label) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error(std::string(label) + ": GL " + std::to_string(error));
}
std::vector<float> Pixels(GLuint texture, int width, int height) {
    std::vector<float> pixels(size_t(width)*height*4);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
    NoError("read image");
    for(float p:pixels) Require(std::isfinite(p),"Non-finite post-process output");
    return pixels;
}
double Difference(const std::vector<float>& a, const std::vector<float>& b) {
    Require(a.size()==b.size(),"image dimensions");
    double sum=0;
    for(size_t i=0;i<a.size();i+=4) for(int c=0;c<3;++c) sum+=std::abs(a[i+c]-b[i+c]);
    return sum/(a.size()/4*3);
}
void Save(const fs::path& file,const std::vector<float>& pixels,int w,int h) {
    std::ofstream out(file,std::ios::binary);out<<"P6\n"<<w<<" "<<h<<"\n255\n";
    for(int y=h-1;y>=0;--y) for(int x=0;x<w;++x) for(int c=0;c<3;++c) {
        uint8_t value=uint8_t(std::clamp(pixels[(y*w+x)*4+c],0.f,1.f)*255);
        out.write(reinterpret_cast<char*>(&value),1);
    }
}
int main(int argc,char** argv) {
    try {
        const fs::path output=argc>1?fs::path(argv[1]):fs::path(".");
        fs::create_directories(output);
        Window window;
        auto* handle=window.makeWindow(Modularity::GraphicsBackend::OpenGL);
        Require(handle,"Create GL context");glfwHideWindow(handle);
        std::cout<<glGetString(GL_RENDERER)<<'\n';
        {
            Renderer renderer;renderer.initialize();renderer.resize(320,240);
            renderer.setAmbientColor(glm::vec3(.65f));
            std::vector<SceneObject> scene;
            auto cube=[&](const char* name,glm::vec3 pos,glm::vec3 scale,glm::vec3 color) {
                SceneObject object(name,ObjectType::Cube,int(scene.size()+1));
                object.hasRenderer=true;object.renderType=RenderType::Cube;
                object.position=pos;object.scale=scale;object.material.color=color;
                object.material.ambientStrength=1.f;scene.push_back(object);
            };
            cube("Floor",{0,-1.1f,0},{12,.2f,12},{.7f,.65f,.55f});
            cube("Wall",{0,2,-3},{12,6,.2f},{.6f,.7f,.8f});
            cube("Box",{0,-.4f,0},{1.2f,1.2f,1.2f},{.8f,.3f,.2f});
            for(int i=-8;i<=8;++i) cube("Stripe",{i*.25f,.2f,-2.8f},{.08f,2,.08f},{.1f,.15f,.25f});
            SceneObject volume("PostFX",ObjectType::PostFXNode,100);
            volume.hasPostFX=true;volume.postFx.bloomEnabled=false;
            volume.postFx.hdrEnabled=false;volume.postFx.gamma=1.f;
            volume.postFx.aoRadius=.0065f;volume.postFx.aoStrength=1.f;
            scene.push_back(volume);
            Camera camera;camera.position={0,.3f,4};camera.front=glm::normalize(glm::vec3(0,-.12f,-1));
            camera.solidBackground=true;camera.backgroundColor={.2f,.3f,.4f};
            uint64_t serial=1;
            auto tick=[&] {glfwSetTime(double(serial)*.04);renderer.setFrameSerial(serial++);};
            auto preview=[&](int slot=0,int w=320,int h=240) {
                tick();GLuint texture=renderer.renderScenePreview(camera,scene,w,h,60,.1f,100,true,slot);
                Require(texture,"Preview output");NoError("preview");return Pixels(texture,w,h);
            };
            const auto baseline=preview();
            scene.back().postFx.ambientOcclusionEnabled=true;
            auto occluded=preview();
            Require(Difference(baseline,occluded)>.0002,"AO does not affect scene geometry");
            Save(output/"ao-off.ppm",baseline,320,240);Save(output/"ao-on.ppm",occluded,320,240);
            renderer.setColorPrecision(RendererColorPrecision::SDR8);
            auto sdrAO=preview();scene.back().postFx.ambientOcclusionEnabled=false;
            auto sdrBase=preview();Require(Difference(sdrAO,sdrBase)>.0002,"SDR8 loses AO depth precision");
            renderer.setColorPrecision(RendererColorPrecision::Auto);
            std::cout<<"PASS: real scene AO and floating-point AO targets with SDR8 color\n";

            scene.back().postFx.motionBlurEnabled=true;scene.back().postFx.motionBlurStrength=1.f;
            preview(10);Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"First preview uses stale history");
            auto stationary=preview(10);Require(renderer.getLastPreviewPostStats().motionBlurUsed,"Preview history missing");
            scene.back().postFx.motionBlurEnabled=false;auto sharp=preview(11);
            Require(Difference(stationary,sharp)<.00001,"Stationary camera must remain sharp");
            scene.back().postFx.motionBlurEnabled=true;
            preview(11);Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"New camera history not reset");
            camera.position.x+=.18f;auto blurred=preview(10);
            Require(renderer.getLastPreviewPostStats().motionBlurUsed,"Moving camera missing history");
            scene.back().postFx.motionBlurEnabled=false;auto movingSharp=preview(12);
            Require(Difference(blurred,movingSharp)>.0001,"Real camera image not blurred");
            Save(output/"motion-on.ppm",blurred,320,240);Save(output/"motion-off.ppm",movingSharp,320,240);
            scene.back().postFx.motionBlurEnabled=true;
            preview(10);preview(10,300,220);
            Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"Resize did not invalidate history");
            preview(10,300,220);Require(renderer.getLastPreviewPostStats().motionBlurUsed,"Resize history did not recover");
            camera.position.x+=3;preview(10,300,220);
            Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"Teleport did not invalidate history");
            camera.position.x-=3;preview(10,300,220);preview(10,300,220);
            const glm::vec3 oldFront=camera.front;camera.front={1,0,0};preview(10,300,220);
            Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"Rotation cut did not reset history");
            camera.front=oldFront;preview(10,300,220);preview(10,300,220);
            serial+=10;preview(10,300,220);
            Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"Long frame gap did not reset history");
            std::cout<<"PASS: preview history, stationary/moving image, resize, camera cuts and time gaps\n";

            GLuint fbo=0,texture=0;glGenFramebuffers(1,&fbo);glGenTextures(1,&texture);
            glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,320,240,0,GL_RGBA,GL_FLOAT,nullptr);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
            Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"External target");
            auto eye=[&](int index,float shift=0.f,float projectionShift=0.f) {
                Camera eyeCam=camera;eyeCam.position.x+=(index==0?-.032f:.032f)+shift;
                glm::mat4 projection=BuildCameraProjection(eyeCam,320,240,60,.1f,100);
                projection[2][0]=(index==0?-.12f:.12f)+projectionShift;
                tick();glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);
                renderer.renderSceneToTarget(eyeCam,scene,fbo,320,240,projection,.1f,100,true,true,index);
                NoError("external eye");return renderer.getLastPreviewPostStats().motionBlurUsed;
            };
            Require(!eye(0),"Left first frame should not use preview history");
            const auto firstLeft=Pixels(texture,320,240);
            Require(!eye(1),"Right first frame should not use left history");
            const auto firstRight=Pixels(texture,320,240);
            Require(eye(0),"Left history missing");
            Require(Difference(firstLeft,Pixels(texture,320,240))<.00001,"Left stationary image inherited right-eye velocity");
            Require(eye(1),"Right history missing");
            Require(Difference(firstRight,Pixels(texture,320,240))<.00001,"Right stationary image inherited left-eye velocity");
            Require(eye(0,.1f),"Left moving history missing");
            Require(eye(1,.1f),"Right moving history missing");
            auto eyePixels=Pixels(texture,320,240);
            Require(!eye(0,.1f,.3f),"Projection change did not reset eye");
            Require(eye(1,.1f),"Left reset should not invalidate right");
            Require(!eye(-1),"Anonymous external target should not share history");
            // A display pass must not inherit the last eye's projection or history.
            tick();renderer.beginRender(camera.getViewMatrix(),BuildCameraProjection(camera,320,240,60,.1f,100),camera.position);
            renderer.renderScene(camera,scene,-1,60,.1f,100,false,nullptr,SceneRenderMode::Normal,true);
            Require(!renderer.getLastViewportPostStats().motionBlurUsed,"Desktop inherited eye history");
            NoError("desktop");
            camera.position.x+=.1f;tick();
            renderer.beginRender(camera.getViewMatrix(),BuildCameraProjection(camera,320,240,60,.1f,100),camera.position);
            renderer.renderScene(camera,scene,-1,60,.1f,100,false,nullptr,SceneRenderMode::Normal,true);
            Require(renderer.getLastViewportPostStats().motionBlurUsed,"Desktop camera history missing");
            glBindTexture(GL_TEXTURE_2D,renderer.getSceneDepthTexture());
            GLint depthFilter=0;glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&depthFilter);
            Require(depthFilter==GL_NEAREST,"Motion blur did not restore nearest depth sampling");
            // UI-only effects must not consume a stale scene depth texture, and
            // postprocessing must restore the raster state its caller supplied.
            glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE);
            glDisable(GL_DEPTH_TEST);glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,1);
            GLuint uiResult=renderer.postProcessTexture(camera,scene,texture,320,240,false);
            Require(uiResult!=0,"Color-only postprocess output");
            Require(!renderer.getLastPreviewPostStats().motionBlurUsed,"Color-only target uses stale depth");
            Require(glIsEnabled(GL_BLEND)&&!glIsEnabled(GL_DEPTH_TEST)&&glIsEnabled(GL_SCISSOR_TEST),"Post raster state not restored");
            glDisable(GL_SCISSOR_TEST);
            const double colorOnlyDifference=Difference(Pixels(uiResult,320,240),Pixels(texture,320,240));
            std::cout<<"Color-only FP16 mean error: "<<colorOnlyDifference<<'\n';
            Require(colorOnlyDifference<.0005,"Scissor/depth contaminated color-only postprocess");
            NoError("color-only state");
            glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);
            std::cout<<"PASS: independent asymmetric left/right/desktop histories and projection resets\n";
            glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);
        }
        NoError("renderer cleanup");
        std::cout<<"PASS: full renderer post-FX integration\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
