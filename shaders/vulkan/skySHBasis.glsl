#ifndef SKY_SH_BASIS_GLSL
#define SKY_SH_BASIS_GLSL
// Orthonormal real SH, bands 0..2, with the same Cartesian convention on CPU.
// Lambert convolution multiplies the bands by 1, 2/3, 1/4 for E/pi.
void skySHBasis(vec3 d,out float b[9]) {
 b[0]=.2820947918;b[1]=.4886025119*d.y;b[2]=.4886025119*d.z;b[3]=.4886025119*d.x;
 b[4]=1.0925484306*d.x*d.y;b[5]=1.0925484306*d.y*d.z;
 b[6]=.3153915653*(3*d.z*d.z-1);b[7]=1.0925484306*d.x*d.z;b[8]=.5462742153*(d.x*d.x-d.y*d.y);
}
#endif
