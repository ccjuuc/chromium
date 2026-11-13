import{C as e,a as t,c as n,i as r,l as i,o as a,s as o,w as s}from"./app-root-DM8MbvWa.js";var c=8e-5,l=5.5;function u(e){return l-Math.max(0,Math.min(100,e))/100*5}function d(e,t,n,r){let i=Math.max(n,.05),a=Math.min(Math.max(r,0),.05),o=2*Math.PI/i,s=e.value-t,c=e.velocity+o*s,l=Math.exp(-o*a);return{value:t+(s+c*a)*l,velocity:(e.velocity-o*c*a)*l}}function f(e,t,n,r,i){if(n.width<=0||n.height<=0)return{x:0,y:0};let a=Math.max(-1,Math.min(1,(e-n.left)/n.width*2-1)),o=Math.max(-1,Math.min(1,1-(t-n.top)/n.height*2));if(!i)return{x:a*r.x,y:o*r.y};let s=o>=0?i.upward:i.downward,c=Math.sign(o)*Math.abs(o)**i.verticalPower*s;return{x:a*r.x,y:c}}function p(e,t){return Math.abs(e.value-t)<c&&Math.abs(e.velocity)<c}var m=`#version 300 es
const vec2 POSITIONS[3] = vec2[3](
  vec2(-1.0, -1.0),
  vec2(3.0, -1.0),
  vec2(-1.0, 3.0)
);

void main() {
  gl_Position = vec4(POSITIONS[gl_VertexID], 0.0, 1.0);
}
`,h=`#version 300 es
precision highp float;

out vec4 outColor;

uniform vec2 u_resolution;
uniform vec2 u_glow_offset;
uniform vec3 u_cell;
uniform float u_lower_falloff;
uniform vec4 u_blob_geometry[8];
uniform vec4 u_blob_color[8];
uniform vec4 u_envelope_geometry[2];
uniform vec4 u_envelope_params[2];

struct GradientSample {
  vec3 premul;
  float alpha;
  float energy;
};

float roundedBoxSdf(vec2 point, vec2 halfSize, float radius) {
  vec2 q = abs(point) - halfSize + radius;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float sampleEnvelope(vec2 uv) {
  float envelopeSum = 0.0;
  for (int index = 0; index < 2; index++) {
    vec4 geometry = u_envelope_geometry[index];
    vec4 parameters = u_envelope_params[index];
    float verticalRadius = geometry.w * mix(
      parameters.x,
      1.0,
      step(geometry.y, uv.y)
    );
    float verticalDelta = (uv.y - geometry.y) / max(verticalRadius, 0.0001);
    float widthProgress = pow(
      1.0 - smoothstep(0.0, 1.0, clamp(verticalDelta, 0.0, 1.0)),
      1.5
    );
    float belowProgress = smoothstep(
      0.0,
      1.0,
      clamp(-verticalDelta, 0.0, 1.0)
    );
    float lowerWidthScale = parameters.z +
      (parameters.z - 1.0) * 1.5 * belowProgress;
    float horizontalRadius = geometry.z * mix(
      1.0,
      lowerWidthScale,
      widthProgress
    );
    vec2 radius = vec2(horizontalRadius, verticalRadius);
    vec2 q = (uv - geometry.xy) / max(radius, vec2(0.0001));
    float shape = pow(abs(q.x), parameters.w) + q.y * q.y;
    envelopeSum += exp(-shape * 2.15) * parameters.y;
  }
  return envelopeSum;
}

GradientSample sampleGradient(vec2 uv) {
  vec2 glowUv = uv - u_glow_offset;
  vec3 colorSum = vec3(0.0);
  float weightSum = 0.0;

  for (int index = 0; index < 8; index++) {
    vec4 geometry = u_blob_geometry[index];
    vec4 blobColor = u_blob_color[index];
    float verticalRadius = geometry.w * mix(
      u_lower_falloff,
      1.0,
      step(geometry.y, glowUv.y)
    );
    vec2 radius = vec2(geometry.z, verticalRadius);
    vec2 q = (glowUv - geometry.xy) / max(radius, vec2(0.0001));
    float weight = exp(-dot(q, q) * 2.15) * blobColor.a;
    colorSum += blobColor.rgb * weight;
    weightSum += weight;
  }

  // Translate the entire color field while the glass-cell geometry stays fixed.
  float movingAlpha = min(sampleEnvelope(glowUv), 0.96);
  float staticWallEnergy = min(sampleEnvelope(uv), 0.96);
  float wallEnergy = max(movingAlpha, staticWallEnergy * 0.24);
  vec3 mixedColor = colorSum / max(weightSum, 0.0001);
  return GradientSample(
    mixedColor * movingAlpha,
    movingAlpha,
    wallEnergy
  );
}

vec2 roundedBoxNormal(vec2 point, vec2 halfSize, float radius) {
  float epsilon = 0.8;
  float right = roundedBoxSdf(
    point + vec2(epsilon, 0.0),
    halfSize,
    radius
  );
  float left = roundedBoxSdf(
    point - vec2(epsilon, 0.0),
    halfSize,
    radius
  );
  float top = roundedBoxSdf(
    point + vec2(0.0, epsilon),
    halfSize,
    radius
  );
  float bottom = roundedBoxSdf(
    point - vec2(0.0, epsilon),
    halfSize,
    radius
  );
  vec2 gradient = vec2(right - left, top - bottom);
  return gradient * inversesqrt(max(dot(gradient, gradient), 0.0001));
}

float noise(vec2 point) {
  return fract(sin(dot(point, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
  vec2 fragment = gl_FragCoord.xy;
  vec2 uv = fragment / u_resolution;
  GradientSample background = sampleGradient(uv);
  float columnCount = 23.0;
  float targetCellSize = u_resolution.x / columnCount;
  float rowCount = max(1.0, round(u_resolution.y / targetCellSize));
  vec2 cellSize = u_resolution / vec2(columnCount, rowCount);
  float opticalCellSize = min(cellSize.x, cellSize.y);
  vec2 grid = fragment / cellSize;
  vec2 cellIndex = floor(grid);
  vec2 local = (fract(grid) - 0.5) * cellSize;
  vec2 halfSize = cellSize * 0.5;
  float radius = opticalCellSize * 0.145;
  float distanceToCell = roundedBoxSdf(local, halfSize, radius);
  float antialias = max(fwidth(distanceToCell), 0.65);
  float inside = 1.0 - smoothstep(
    -antialias * 0.35,
    antialias * 0.85,
    distanceToCell
  );

  float insetDepth = max(-distanceToCell, 0.0);
  float bevel = 1.0 - smoothstep(
    0.0,
    opticalCellSize * 0.035,
    insetDepth
  );
  vec2 edgeNormal = roundedBoxNormal(local, halfSize, radius);

  vec2 cellCenter = (cellIndex + 0.5) * cellSize;
  vec2 cellCenterUv = cellCenter / u_resolution;
  vec2 magnifiedUv = cellCenterUv + (uv - cellCenterUv) * 0.28;
  magnifiedUv -= edgeNormal * bevel * (vec2(0.5) / u_resolution);
  GradientSample refracted = sampleGradient(magnifiedUv);
  GradientSample centered = sampleGradient(cellCenterUv);
  vec2 frostStep = cellSize / u_resolution * 0.24;
  GradientSample frostLeft = sampleGradient(magnifiedUv - vec2(frostStep.x, 0.0));
  GradientSample frostRight = sampleGradient(magnifiedUv + vec2(frostStep.x, 0.0));
  GradientSample frostDown = sampleGradient(magnifiedUv - vec2(0.0, frostStep.y));
  GradientSample frostUp = sampleGradient(magnifiedUv + vec2(0.0, frostStep.y));

  vec3 diffusedPremul = (
    refracted.premul * 4.0 +
    frostLeft.premul +
    frostRight.premul +
    frostDown.premul +
    frostUp.premul
  ) / 8.0;
  float diffusedAlpha = (
    refracted.alpha * 4.0 +
    frostLeft.alpha +
    frostRight.alpha +
    frostDown.alpha +
    frostUp.alpha
  ) / 8.0;
  vec3 cellPremul = mix(diffusedPremul, centered.premul, 0.12);
  float cellAlpha = mix(diffusedAlpha, centered.alpha, 0.12);
  cellPremul = mix(cellPremul, u_cell * cellAlpha, 0.025);

  float wallStrength = mix(
    background.energy,
    centered.energy,
    0.65
  );
  float glassWallFade = smoothstep(0.0005, 0.035, wallStrength);
  float neutralGlassAlpha = glassWallFade * 0.008;
  cellPremul += u_cell * neutralGlassAlpha * (1.0 - cellAlpha);
  cellAlpha += neutralGlassAlpha * (1.0 - cellAlpha);

  float topLeftEdge = max(
    dot(edgeNormal, normalize(vec2(-0.72, 0.69))),
    0.0
  ) * bevel;
  float bottomRightEdge = max(
    dot(edgeNormal, normalize(vec2(0.72, -0.69))),
    0.0
  ) * bevel;

  float highlight = bevel * 0.012 + bottomRightEdge * 0.008;
  cellPremul = mix(
    cellPremul,
    vec3(cellAlpha),
    clamp(highlight, 0.0, 0.025)
  );
  float shadowAlpha = topLeftEdge * glassWallFade * 0.004;
  cellPremul *= 1.0 - shadowAlpha;
  cellAlpha += shadowAlpha * (1.0 - cellAlpha);
  float frostNoise = (noise(fragment) - 0.5) / 255.0;
  cellPremul += vec3(frostNoise) * cellAlpha * 0.55;

  float glassMix = inside * glassWallFade;
  vec3 premul = mix(
    background.premul,
    cellPremul,
    glassMix
  );
  float alpha = mix(background.alpha, cellAlpha, glassMix);
  vec3 straightColor = alpha > 0.0001
    ? premul / alpha
    : vec3(0.0);

  outColor = vec4(
    clamp(straightColor, 0.0, 1.0),
    clamp(alpha, 0.0, 0.96)
  );
}
`;function g(e,t,n){let r=e.createShader(t);return r?(e.shaderSource(r,n),e.compileShader(r),e.getShaderParameter(r,e.COMPILE_STATUS)?r:(e.deleteShader(r),null)):null}function _(e){let t=g(e,e.VERTEX_SHADER,m),n=g(e,e.FRAGMENT_SHADER,h);if(!t||!n)return t&&e.deleteShader(t),n&&e.deleteShader(n),null;let r=e.createProgram();return r?(e.attachShader(r,t),e.attachShader(r,n),e.linkProgram(r),e.deleteShader(t),e.deleteShader(n),e.getProgramParameter(r,e.LINK_STATUS)?r:(e.deleteProgram(r),null)):(e.deleteShader(t),e.deleteShader(n),null)}function v(e,t){let n={resolution:e.getUniformLocation(t,`u_resolution`),glowOffset:e.getUniformLocation(t,`u_glow_offset`),cell:e.getUniformLocation(t,`u_cell`),lowerFalloff:e.getUniformLocation(t,`u_lower_falloff`),blobGeometry:e.getUniformLocation(t,`u_blob_geometry[0]`),blobColor:e.getUniformLocation(t,`u_blob_color[0]`),envelopeGeometry:e.getUniformLocation(t,`u_envelope_geometry[0]`),envelopeParams:e.getUniformLocation(t,`u_envelope_params[0]`)};return Object.values(n).some(e=>e===null)?null:n}function y(e,t,n){let r=e.getPropertyValue(t).trim().split(/[\s,]+/).map(Number);if(r.length<3||r.slice(0,3).some(e=>!Number.isFinite(e)))return n;let[i,a,o]=r;return i===void 0||a===void 0||o===void 0?n:[i/255,a/255,o/255]}function b(e){let t=getComputedStyle(e);return{cell:y(t,`--cubic-glass-cell-rgb`,r.cell),blue:y(t,`--cubic-glass-blue-rgb`,r.blue),cyan:y(t,`--cubic-glass-cyan-rgb`,r.cyan),violet:y(t,`--cubic-glass-violet-rgb`,r.violet),pink:y(t,`--cubic-glass-pink-rgb`,r.pink),warm:y(t,`--cubic-glass-warm-rgb`,r.warm)}}function x(e,t,n){e.uniform3f(t,n[0],n[1],n[2])}function S(e,n,r,i){let a=new Float32Array(32),o=new Float32Array(32),s=new Float32Array(8),c=new Float32Array(8),l=t[i];for(let[e,t]of l.blobs.entries()){let n=e*4,i=r[t.color];a.set([...t.center,...t.radius],n),o.set([...i,t.intensity],n)}for(let[e,t]of l.envelopes.entries())s.set([...t.center,...t.radius],e*4),c.set([t.lowerFalloff,t.intensity,t.lowerWidthScale,t.horizontalPower],e*4);e.uniform4fv(n.blobGeometry,a),e.uniform4fv(n.blobColor,o),e.uniform4fv(n.envelopeGeometry,s),e.uniform4fv(n.envelopeParams,c),e.uniform1f(n.lowerFalloff,l.lowerFalloff)}function C(e){let t=null;try{t=e.getContext(`webgl2`,{alpha:!0,antialias:!1,depth:!1,powerPreference:`low-power`,premultipliedAlpha:!1,preserveDrawingBuffer:!1,stencil:!1})}catch{return null}if(!t)return null;let n=t,r=_(n),i=n.createVertexArray();if(!r||!i)return r&&n.deleteProgram(r),i&&n.deleteVertexArray(i),null;let a=v(n,r);return a?{render({height:t,offsetX:o,offsetY:s,preset:c,refreshScene:l,width:u}){if((e.width!==u||e.height!==t)&&(e.width=u,e.height=t),n.viewport(0,0,u,t),n.useProgram(r),n.bindVertexArray(i),n.uniform2f(a.resolution,u,t),n.uniform2f(a.glowOffset,o,s),l){let t=b(e);x(n,a.cell,t.cell),S(n,a,t,c)}n.drawArrays(n.TRIANGLES,0,3)},dispose(e=!1){e||(n.deleteVertexArray(i),n.deleteProgram(r))}}:(n.deleteVertexArray(i),n.deleteProgram(r),null)}var ee={dispose:()=>{},schedule:()=>{}};function w(t,r,c,l,m){let h=C(t);if(!h)return ee;let g=h,_=0,v=!0,y=0,b=0,x=!1,S=null,w=null,T=0,E={height:0,left:0,top:0,width:0},D=!0,O=0,k=0,A=!1,j=0,M=0,N={value:0,velocity:0},P={value:0,velocity:0},F=m()??r.parentElement??r,I=()=>{let t=l();return t.enabled&&t.pointerFollow&&!e()&&w?.matches!==!1},L=()=>p(N,j)&&p(P,M),R=e=>{A!==e&&(A=e,r.dataset.pointerActive=String(e))},z=(e=!1)=>{j=0,M=0,R(!1),e&&(N={value:0,velocity:0},P={value:0,velocity:0},T=0)},B=()=>{let e=r.getBoundingClientRect();E={height:e.height,left:e.left,top:e.top,width:e.width},b=e.width,y=e.height,v=!1},V=()=>{if(!A)return;let e=c(),[t,n]=o[e],r=l(),i=f(O,k,E,{x:t,y:n},r.positionConstraint?a[e]:void 0);j=i.x,M=i.y};function H(){x||_!==0||document.visibilityState===`hidden`||(_=requestAnimationFrame(U))}function U(e){if(_=0,x||(v&&B(),b<1||y<1))return;if(I()||z(!0),V(),!L()){let t=T===0?1/60:(e-T)/1e3,r=u(l().horizontalSpeed),a=A?r:r*.88,o=A?n:i;N=d(N,j,a,t),P=d(P,M,o,t),L()&&(N={value:j,velocity:0},P={value:M,velocity:0})}T=e;let a=!L(),o=Math.min(window.devicePixelRatio||1,2),s=Math.sqrt((a?2e6:5e6)/(b*y)),f=Math.min(o,s),p=Math.max(1,Math.round(b*f)),m=Math.max(1,Math.round(y*f));g.render({height:m,offsetX:N.value,offsetY:P.value,preset:c(),refreshScene:D,width:p}),D=!1,r.dataset.renderer=`webgl2`,t.dataset.ready=`true`,a?H():T=0}let W=()=>{D=!0,I()||z(e()),H()},G=()=>{v=!0,W()},K=e=>{if(!(e.pointerType&&e.pointerType!==`mouse`||!I())){if(v&&B(),!(e.clientX>=E.left&&e.clientX<=E.left+E.width&&e.clientY>=E.top&&e.clientY<=E.top+E.height)){let e=A||!L();z(),e&&H()}else O=e.clientX,k=e.clientY,R(!0),V(),H()}},q=()=>{let e=A||!L();z(),e&&H()},J=typeof ResizeObserver>`u`?null:new ResizeObserver(G);J?.observe(r);let Y=typeof MutationObserver>`u`?null:new MutationObserver(W);Y?.observe(document.documentElement,{attributeFilter:[`class`],attributes:!0});let X=()=>{Z(),G()},Z=()=>{S?.removeEventListener(`change`,X),S=typeof window.matchMedia==`function`?window.matchMedia(`(resolution: ${window.devicePixelRatio||1}dppx)`):null,S?.addEventListener(`change`,X)},te=()=>{e()&&z(!0),W()},Q=()=>{w?.matches||z(!0),W()},$=()=>{document.visibilityState===`hidden`?(_!==0&&cancelAnimationFrame(_),_=0,z(!0)):(T=0,W())};Z();let ne=s(te);return typeof window.matchMedia==`function`&&(w=window.matchMedia(`(hover: hover) and (pointer: fine)`),w.addEventListener(`change`,Q)),F.addEventListener(`pointerleave`,q),F.addEventListener(`pointercancel`,q),window.addEventListener(`pointermove`,K),window.addEventListener(`blur`,q),window.addEventListener(`resize`,G),document.addEventListener(`visibilitychange`,$),W(),{dispose:(e=!1)=>{x||(x=!0,_!==0&&cancelAnimationFrame(_),J?.disconnect(),Y?.disconnect(),S?.removeEventListener(`change`,X),ne(),w?.removeEventListener(`change`,Q),F.removeEventListener(`pointerleave`,q),F.removeEventListener(`pointercancel`,q),window.removeEventListener(`pointermove`,K),window.removeEventListener(`blur`,q),window.removeEventListener(`resize`,G),document.removeEventListener(`visibilitychange`,$),g.dispose(e))},schedule:W}}export{w as startCubicGlassRenderer};