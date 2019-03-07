#include "scene.h"
#include "camera.h"
#include "bvh.h"
#include "maths.h"
#include "render.h"
#include "loader.h"
#include "util.h"
#include "png.h"
#include "nlm.h"
#include "disney.h"
#include "sampler.h"

#if _WIN32

#include "freeglut/include/GL/glut.h"
#include <cuda_runtime.h>

#elif __APPLE__

#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED 
#include <opengl/gl3.h>
#include <glut/glut.h>

#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLRenderers.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/OpenGL.h>

#endif
#include <GL/gl.h>
#include <GL/freeglut.h>


#include <iostream>

using namespace std;

Vec3 g_camPos;
Vec3 g_camAngle;
Mat44 g_camTransform;

float g_flySpeed = 0.5f;
bool g_flyMode = false;

// renderer
Renderer* g_renderer;

// the main scene
Scene g_scene;
Options g_options;
Camera g_camera;

// output buffers
Color* g_pixels;
Color* g_filtered;
Color* g_exposed;

// total sample count so far
int g_sampleCount;

// nonlinear means filter
float g_nlmFalloff = 200.0f;
int g_nlmWidth = 0;

// in batch mode keep track of current file
bool g_batchMode = false;
int g_batchIndex = 0;

int g_argc;
char** g_argv;

const char* g_outputFile;

double GetSeconds();

void InitFrameBuffer()
{
    delete[] g_pixels;
    delete[] g_filtered;
    delete[] g_exposed;

    g_pixels = new Color[g_options.width*g_options.height];
    g_filtered = new Color[g_options.width*g_options.height];
    g_exposed = new Color[g_options.width*g_options.height];

    g_sampleCount = 0;

	printf("%d %d\n", g_options.width, g_options.height);

	g_renderer->Init(g_options.width, g_options.height);
}

#include "tests/testMaterials.h"
#include "tests/testMesh.h"
#include "tests/testVeach.h"
#include "tests/testMotionBlur.h"

void ProcessCommandLine(int argc, char* argv[])
{  
   // the last argument should be the input file
    const char* filename = NULL;

    if (argc > 1)
        filename = argv[argc-1];

    if (filename)
    {
        // if filename contains a % character treat as a printf string
        char batchFile[2048];
        if (strchr(filename, '%'))
        {
            sprintf(batchFile, filename, g_batchIndex);
            filename = batchFile;

            // set output file as input + .png extension
            static char output[2048];
            sprintf(output, "%s.png", filename);        
            g_outputFile = output;

            g_batchMode = true;
        }

        bool success = false;

        if (strcmp(strrchr(filename, '.'), ".tin") == 0)
            success = LoadTin(filename, &g_scene, &g_camera, &g_options);
        
        if (strcmp(strrchr(filename, '.'), ".json") == 0)
            success = LoadTungsten(filename, &g_scene, &g_camera, &g_options);        

        if (!success)
        {
            printf("Couldn't open %s for reading.\n", filename);
            exit(-1);
        }
    }
    else
    {
        // default test scene
        //TestVeach(&g_scene, &g_camera, &g_options);
        //TestMaterials(&g_scene, &g_camera, &g_options);
		TestPaniq(&g_scene, &g_camera, &g_options);
    }

    // cmdline option overrides
	for (int i=1; i < argc; ++i)
	{
		sscanf(argv[i], "-spp=%d", &g_options.maxSamples);
		sscanf(argv[i], "-width=%d", &g_options.width);
		sscanf(argv[i], "-height=%d", &g_options.height);
		sscanf(argv[i], "-exposure=%f", &g_options.exposure);
		sscanf(argv[i], "-maxdepth=%d", &g_options.maxDepth);

        // convert a mesh to flat binary format
        if (strstr(argv[i], "-convert") && filename)
        {
            Mesh* m = ImportMesh(filename);
            if (m)
            {
                const char* extension = strrchr(filename, '.');

                std::string s(filename, extension);
                s += ".bin";

                ExportMeshToBin(s.c_str(), m);
                exit(0);
            }
            else
            {
                printf("Could open mesh %s for conversion\n", filename);
                exit(-1);
            }
        }              
	}
}

void Init(int argc, char* argv[])
{
	double start = GetSeconds();
	
	g_scene.Clear();

    // set up defaults
    g_options.width = 512;
    g_options.height = 256;
    g_options.filter = Filter(eFilterGaussian, 0.75f, 1.0f);
    g_options.mode = ePathTrace;
    g_options.exposure = 1.0f;
    g_options.limit = 1.5f;
	g_options.clamp = FLT_MAX;
	g_options.maxDepth = 4;
	g_options.maxSamples = std::numeric_limits<int>::max();

    g_camera.position = Vec3(0.0f, 1.0f, 5.0f);
    g_camera.rotation = Quat();
    g_camera.fov = DegToRad(35.0f);

    // allow command line to override options
    ProcessCommandLine(argc, argv);

    // initialize scene
    g_scene.Build();

	// set fly cam to scene cam
	g_camPos = g_camera.position;	


#if _WIN32
    // create renderer
    g_renderer = CreateGpuRenderer(&g_scene);
	//g_renderer = CreateNullRenderer(&g_scene);
	//g_renderer = CreateGpuWavefrontRenderer(&g_scene);
#else
    g_renderer = CreateCpuRenderer(&g_scene);
	//g_renderer = CreateCpuWavefrontRenderer(&g_scene);
#endif

	double end = GetSeconds();

	printf("Created renderer in %fms\n", (end-start)*1000.0f);

    InitFrameBuffer();
}


void Render()
{
    Camera camera;

    if (g_flyMode)
    {
        camera.position = g_camPos;
        camera.rotation = Quat(Vec3(0.0f, 1.0f, 0.0f), g_camAngle.x)*Quat(Vec3(1.0f, 0.0f, 0.0f), g_camAngle.y);
        camera.fov = DegToRad(35.f);

        g_camTransform = Transform(camera.position, camera.rotation);
    }
    else
    {
        camera = g_camera;
    }
    
	double startTime = GetSeconds();

	const int numSamples = 16;

	if (g_sampleCount < g_options.maxSamples)
	{
		for (int i=0; i < numSamples; ++i)
		{
			// take one more sample per-pixel each frame for progressive rendering
			g_renderer->Render(camera, g_options, g_pixels);
		}
	}

	double endRenderTime = GetSeconds();

	// copy to frame buffer
    Color* presentMem = g_pixels;

    g_sampleCount += numSamples;

    if (g_options.mode == ePathTrace)
    {
        int numPixels = g_options.width*g_options.height;

        for (int i=0; i < numPixels; ++i)
        {            
            //assert(g_pixels[i].w > 0.0f);

            float s = g_options.exposure / g_pixels[i].w;

            g_filtered[i] = LinearToSrgb(ToneMap(g_pixels[i] * s, g_options.limit));
        }

        if (g_nlmWidth)
        {
            NonLocalMeansFilter(g_filtered, g_exposed, g_options.width, g_options.height, g_nlmFalloff, g_nlmWidth);
            presentMem = g_exposed;
        }
        else
        {
            presentMem = g_filtered;
        }
    }

	// present in interactive mode
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
	glViewport(0, 0, g_options.width, g_options.height);
	
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, g_options.width, 0.0f, g_options.height, -1.0f, 1.0f);
	
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glPixelZoom(1.0f, -1.0f);
	glRasterPos2f(0, g_options.height);

    glDrawPixels(g_options.width, g_options.height, GL_RGBA, GL_FLOAT, presentMem);

	double endFrameTime = GetSeconds();

	printf("%d render: (%.4fms) total: (%.4fms)\n", g_sampleCount, (endRenderTime-startTime)*1000.0f, (endFrameTime-startTime)*1000.0f);
	fflush(stdout);

	// output frame to file if finished
	if (g_sampleCount >= g_options.maxSamples)
	{
		if (g_outputFile)
		{
			WritePng(g_filtered, g_options.width, g_options.height, g_outputFile);
		}

		// finished rendering, output file and move onto next in batch
		if (g_batchMode)
		{
			g_batchIndex++;

			// re-init
			delete g_renderer;
			g_renderer = NULL;

			Init(g_argc, g_argv);
		}
	}
}


void GLUTUpdate()
{
    Render();

	// flip
	glutSwapBuffers();
}

void GLUTReshape(int width, int height)
{
	if (width != g_options.width || height != g_options.height)
	{
		g_options.width = width;
		g_options.height = height;

		InitFrameBuffer();
	}
}

void GLUTArrowKeys(int key, int x, int y)
{
}

void GLUTArrowKeysUp(int key, int x, int y)
{
}

void GLUTKeyboardDown(unsigned char key, int x, int y)
{
    bool resetFrame = false;

 	switch (key)
	{
    case 'w':
        g_camPos -= Vec3(g_camTransform.GetCol(2))*g_flySpeed; resetFrame = true;
		break;
    case 's':
        g_camPos += Vec3(g_camTransform.GetCol(2))*g_flySpeed; resetFrame = true; 
        break;
    case 'a':
        g_camPos -= Vec3(g_camTransform.GetCol(0))*g_flySpeed; resetFrame = true;
        break;
    case 'd':
        g_camPos += Vec3(g_camTransform.GetCol(0))*g_flySpeed; resetFrame = true;
        break;
	case 'f':
		g_flyMode = !g_flyMode;
		break;
	case '1':
		g_options.mode = eNormals;
		break;
	case '2':
		g_options.mode = eComplexity;
		break;
    case '3':
        g_options.mode = ePathTrace; resetFrame = true;
        break;
	case '[':
		g_options.exposure -= 0.01f;
		break;
	case ']':
		g_options.exposure += 0.01f;
		break;
    case 'n':
    {
        if (g_nlmWidth)
            g_nlmWidth = 0;
        else
            g_nlmWidth = 1;
        break;
    }
	case 'i':
	{
		WritePng(g_filtered, g_options.width, g_options.height, "images/output.png");
		break;
	}
	case 'q':
	case 27:
		exit(0);
		break;
	};

    // reset image if there are any camera changes
    if (resetFrame == true)
    {
        InitFrameBuffer();
    }
}

void GLUTKeyboardUp(unsigned char key, int x, int y)
{
// 	switch (key)
// 	{
// 	case 27:
// 		exit(0);
// 		break;
// 	};

}

static int lastx;
static int lasty;

void GLUTMouseFunc(int b, int state, int x, int y)
{
	switch (state)
	{
	case GLUT_UP:
		{
			lastx = x;
			lasty = y;			
		}
	case GLUT_DOWN:
		{
			lastx = x;
			lasty = y;
		}
	}
}

void GLUTMotionFunc(int x, int y)
{
    int dx = x-lastx;
    int dy = y-lasty;

    const float sensitivity = 0.01f;

    g_camAngle.x -= dx*sensitivity;
    g_camAngle.y -= dy*sensitivity;

	lastx = x;
	lasty = y;

    if (g_options.mode == ePathTrace)
    {
        InitFrameBuffer();
    }
}


/*
void Application::JoystickFunc(int x, int y, int z, unsigned long buttons)
{
g_app->JoystickFunc(x, y, z, buttons);
}
*/

int main(int argc, char* argv[])
{	

#if _WIN32 

	const int device = 0;
	cudaSetDevice(device);
	
	cudaDeviceProp props;
	cudaError err = cudaGetDeviceProperties(&props, device);
	
	// require SM .0 GPU
	if (props.major < 5)
	{
		printf("Need a compute capable 5.0 device\n");
		return -1;
	}

	// retrieve device name
	char name[256];
	memcpy((void*)name, props.name, 256);

	printf("Compute device: %s\n", name);

#endif // _WIN32

	g_argc = argc;
	g_argv = argv;

    Init(argc, argv);

	// init gl
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH );

	glutInitWindowSize(g_options.width, g_options.height);
	glutCreateWindow("Tinsel");
	glutPositionWindow(200, 200);

    glutMouseFunc(GLUTMouseFunc);
	glutReshapeFunc(GLUTReshape);
	glutDisplayFunc(GLUTUpdate);
	glutKeyboardFunc(GLUTKeyboardDown);
	glutKeyboardUpFunc(GLUTKeyboardUp);
	glutIdleFunc(GLUTUpdate);	
	glutSpecialFunc(GLUTArrowKeys);
	glutSpecialUpFunc(GLUTArrowKeysUp);
	glutMotionFunc(GLUTMotionFunc);

	glutMainLoop();
}



 