typedef int oldint [[deprecated("use int32")]];
struct [[deprecated]] Legacy { int x; };
oldint gi;
struct Legacy gl;
int main(void) { return gi + gl.x + (int)sizeof(oldint) - 4; }
