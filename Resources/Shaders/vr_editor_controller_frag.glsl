#version 330 core
in vec3 position;
in vec3 normal;
in vec2 uv;
out vec4 FragColor;
uniform sampler2D albedoMap;
uniform sampler2D specularMap;
uniform sampler2D normalMap;
uniform sampler2D ormMap;
uniform bool hasAlbedo;
uniform bool hasSpecular;
uniform bool hasNormal;
uniform bool hasOrm;
uniform vec3 eye;
uniform float metallic;
uniform float roughness;
uniform float ambient;
void main() {
    vec3 base=hasAlbedo?pow(texture(albedoMap,uv).rgb,vec3(2.2)):vec3(0.11);
    vec3 n=normalize(normal);
    if(hasNormal) {
        vec3 q1=dFdx(position),q2=dFdy(position);
        vec2 st1=dFdx(uv),st2=dFdy(uv);
        vec3 tangent=q1*st2.y-q2*st1.y;
        if(dot(tangent,tangent)>0.0000001) {
            tangent=normalize(tangent-n*dot(n,tangent));
            vec3 bitangent=normalize(cross(n,tangent));
            n=normalize(mat3(tangent,bitangent,n)*(texture(normalMap,uv).xyz*2.0-1.0));
        }
    }
    vec3 orm=hasOrm?texture(ormMap,uv).rgb:vec3(1,roughness,metallic);
    float r=max(0.04,orm.g),m=orm.b;
    vec3 l=normalize(vec3(-0.4,0.85,0.6));
    vec3 v=normalize(eye-position),h=normalize(l+v);
    vec3 f0=mix(vec3(0.04),base,m);
    if(hasSpecular)f0*=texture(specularMap,uv).rgb;
    float spec=pow(max(dot(n,h),0.0),mix(180.0,4.0,r));
    // Stable editor studio lights keep both controllers readable in dark scenes.
    vec3 color=base*(ambient*orm.r+max(dot(n,l),0.0)*0.9+max(dot(n,-l),0.0)*0.22);
    color+=f0*spec*(1.0-r)*3.0;
    FragColor=vec4(pow(max(color,vec3(0)),vec3(1.0/2.2)),1);
}
