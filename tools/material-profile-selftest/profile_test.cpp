#include "MaterialShaderProfileSelection.h"
#include "EngineMaterialIO.h"
#include "Platform/AssetSource.h"
#include "UnityAssetImport.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
void Check(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
int main(int argc,char** argv) {
 try {
  std::filesystem::path root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
  Modularity::Platform::SetPortableAssetRoot(root.string());
  const auto pack=root/"Surface.modushader";
  std::ofstream(pack)<<"name=Project Surface\nvertex=surface.vert\nfragment=surface.frag\ndepthFragment=surface_depth.frag\ngbufferFragment=surface_gbuffer.frag\nshadowDirectionalVertex=surface_shadow.vert\nshadowCubeVertex=surface_cube.vert\ntwoSided=1\ncutout=1\n"
   "uniform=0|float|customGain|Gain|0.7|control|0,1\n"
   "uniform=1|vec3|customTint|Tint|0.2,0.4,0.6|control|0,1\n"
   "uniform=31|bool|customToggle|Toggle|1|control|0,1\n"
   "uniform=32|float|outOfBounds|Bad|1|control|0,1\n";
  MaterialFileData a,b;SetMaterialShaderProfile(a.props.shaderParameters,pack.string());
  Check(a.props.usesProjectShader(),"register project material");
  const auto& profile=GetMaterialShaderProfile(a.props.shaderParameters.profileId);
  Check(profile.fields.size()==3&&profile.twoSided&&profile.cutout,"metadata and invalid slot");
  Check(profile.vert==(root/"surface.vert").generic_string(),"relative stage resolution");
  Check(profile.depthFrag==(root/"surface_depth.frag").generic_string(),"optional depth stage resolution");
  Check(a.props.shaderParameters.values[0].x==.7f,"project default");
  a.props.shaderParameters.values[1]=glm::vec4(.12f,.34f,.56f,.78f);
  a.props.shaderParameters.values[31].x=0;
  Check(writeMaterialFile(a,(root/"material.mat").string()),"save");
  Check(readMaterialFile((root/"material.mat").string(),b),"load");
  Check(b.props.shaderParameters.profileId==a.props.shaderParameters.profileId,"stable handle");
  for(int i=0;i<32;++i) Check(a.props.shaderParameters.values[i]==b.props.shaderParameters.values[i],"parameter roundtrip");
  const auto preserved=b.props.shaderParameters.values;
  for(int frame=0;frame<5;++frame) {
   Check(!SyncMaterialShaderProfileSelection(b.props.shaderParameters,pack.string()),"absolute picker path must preserve portable material edits");
   Check(b.props.shaderParameters.values==preserved,"editing frame must not reload defaults");
  }
  const auto other=root/"Other.modushader";
  std::filesystem::copy_file(pack,other,std::filesystem::copy_options::overwrite_existing);
  Check(SyncMaterialShaderProfileSelection(b.props.shaderParameters,other.string()),"explicit profile selection changes profile");
  Check(b.props.shaderParameters.values[0].x==.7f,"new profile gets its defaults");
  Check(ParseMaterialShaderValue("nan,inf,3")==glm::vec4(0,0,3,0),"reject nonfinite data");
  std::ofstream(root/"unity.mat")<<"%YAML 1.1\nMaterial:\n";
  Check(!writeMaterialFile(a,(root/"unity.mat").string()),"preserve Unity source material");
  Check(!readMaterialFile((root/"unity.mat").string(),b),"reject Unity source as native");
  std::cout<<"PASS: project declarations, defaults, bounds, finite values, persistence, Unity protection\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
