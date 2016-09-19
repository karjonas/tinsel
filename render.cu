#include "maths.h"
#include "render.h"
#include "disney.h"

struct GPUScene
{
	Primitive* primitives;
	int numPrimitives;

	Primitive* lights;
	int numLights;

	Color sky;
};

struct GPUBVH
{
	Vec3 lower;
	Vec3 upper;

	int leftIndex;
	int rightIndex : 31;
	bool leaf : 1;
};

struct GPUMesh
{
	Vec3* positions;
	Vec3* normals;
	int* indices;
};

// trace a ray against the scene returning the closest intersection
__device__ bool Trace(const GPUScene& scene, const Ray& ray, float& outT, Vec3& outNormal, const Primitive** outPrimitive)
{
	// disgard hits closer than this distance to avoid self intersection artifacts
	const float kEpsilon = 0.001f;

	float minT = REAL_MAX;
	const Primitive* closestPrimitive = NULL;
	Vec3 closestNormal(0.0f);

	for (int i=0; i < scene.numPrimitives; ++i)
	{
		const Primitive& primitive = scene.primitives[i];

		float t;
		Vec3 n;

		if (Intersect(primitive, ray, t, &n))
		{
			if (t < minT && t > kEpsilon)
			{
				minT = t;
				closestPrimitive = &primitive;
				closestNormal = n;
			}
		}
	}
	
	outT = minT;		
	outNormal = closestNormal;
	*outPrimitive = closestPrimitive;

	return closestPrimitive != NULL;
}



__device__ Color SampleLights(const GPUScene& scene, const Primitive& primitive, const Vec3& surfacePos, const Vec3& surfaceNormal, const Vec3& wo, float time, Random& rand)
{	
	Color sum(0.0f);

	for (int i=0; i < scene.numLights; ++i)
	{
		// assume all lights are area lights for now
		const Primitive& lightPrimitive = scene.lights[i];
				
		Color L(0.0f);

		const int numSamples = 2;

		for (int s=0; s < numSamples; ++s)
		{
			// sample light source
			Vec3 lightPos;
			float lightArea;

			Sample(lightPrimitive, lightPos, lightArea, rand);

			Vec3 wi = Normalize(lightPos-surfacePos);

			// check visibility
			float t;
			Vec3 ln;
			const Primitive* hit;
			if (Trace(scene, Ray(surfacePos, wi, time), t, ln, &hit))
			{
				// did we hit a light prim?
				if (hit->light)
				{
					const Color f = BRDF(primitive.material, surfacePos, surfaceNormal, wi, wo);

					// light pdf
					const float nl = Clamp(Dot(ln, -wi), 0.0f, 1.0f);
					
					if (nl > 0.0)
					{
						const float lightPdf = (t*t) / (nl*lightArea);
					
						L += f * hit->material.emission * Clamp(Dot(wi, surfaceNormal), 0.0f, 1.0f)  / lightPdf;
					}
				}
			}		
		}
	
		sum += L / float(numSamples);
	}

	return sum;
}


// reference, no light sampling, uniform hemisphere sampling
__device__ Color ForwardTraceExplicit(const GPUScene& scene, const Vec3& startOrigin, const Vec3& startDir, Random& rand, int maxDepth)
{	
    // path throughput
    Color pathThroughput(1.0f, 1.0f, 1.0f, 1.0f);
    // accumulated radiance
    Color totalRadiance(0.0f);

    Vec3 rayOrigin = startOrigin;
    Vec3 rayDir = startDir;
	float rayTime = rand.Randf();

    float t = 0.0f;
    Vec3 n(rayDir);
    const Primitive* hit;

    for (int i=0; i < maxDepth; ++i)
    {
        // find closest hit
        if (Trace(scene, Ray(rayOrigin, rayDir, rayTime), t, n, &hit))
        {	
            // calculate a basis for this hit point
            Vec3 u, v;
            BasisFromVector(n, &u, &v);

            const Vec3 p = rayOrigin + rayDir*t;

    		// if we hit a light then terminate and return emission
			// first trace is our only chance to add contribution from directly visible light sources
        
			if (i == 0)
			{
				totalRadiance += hit->material.emission;
			}
			
    	    // integral of Le over hemisphere
            totalRadiance += SampleLights(scene, *hit, p, n, -rayDir, rayTime, rand);

            // update position and path direction
            const Vec3 outDir = Mat33(u, v, n)*UniformSampleHemisphere(rand);

            // reflectance
            Color f = BRDF(hit->material, p, n, -rayDir, outDir);

            // update throughput with primitive reflectance
            pathThroughput *= f * Clamp(Dot(n, outDir), 0.0f, 1.0f) / kInv2Pi;

            // update path direction
            rayDir = outDir;
            rayOrigin = p;
        }
        else
        {
            // hit nothing, terminate loop
        	totalRadiance += scene.sky*pathThroughput;
			break;
        }
    }

    return totalRadiance;
}

__global__ void RenderGpu(GPUScene scene, Camera camera, int width, int height, int maxDepth, RenderMode mode, int seed, Color* output)
{
	const int tid = blockDim.x*blockIdx.x + threadIdx.x;

	const int i = tid%width;
	const int j = tid/width;

	if (i < width && j < height)
	{
		Vec3 origin;
		Vec3 dir;

		// initialize a per-thread PRNG
		Random rand(tid + seed);

		if (mode == eNormals)
		{
			GenerateRayNoJitter(camera, i, j, origin, dir);

			const Primitive* p;
			float t;
			Vec3 n;

			if (Trace(scene, Ray(origin, dir, 1.0f), t, n, &p))
			{
				n = n*0.5f+0.5f;
				output[(height-1-j)*width+i] = Color(n.x, n.y, n.z, 1.0f);
			}
			else
			{
				output[(height-1-j)*width+i] = Color(0.5f);
			}
		}
		else if (mode == ePathTrace)
		{
			GenerateRay(camera, i, j, origin, dir, rand);

			//output[(height-1-j)*width+i] += PathTrace(*scene, origin, dir);
			output[(height-1-j)*width+i] += ForwardTraceExplicit(scene, origin, dir, rand, maxDepth);
		}

		/*
		// generate a ray         
		switch (mode)
		{
			case ePathTrace:
			{
				GenerateRay(camera, i, j, origin, dir, rand);

				//output[(height-1-j)*width+i] += PathTrace(*scene, origin, dir);
				output[(height-1-j)*width+i] += ForwardTraceExplicit(scene, origin, dir, rand);
				break;
			}
			case eNormals:
			{
				GenerateRayNoJitter(camera, i, j, origin, dir);

				const Primitive* p;
				float t;
				Vec3 n;

				if (Trace(scene, Ray(origin, dir, 1.0f), t, n, &p))
				{
					n = n*0.5f+0.5f;
					output[(height-1-j)*width+i] = Color(n.x, n.y, n.z, 1.0f);
				}
				else
				{
					output[(height-1-j)*width+i] = Color(0.5f);
				}
				break;
			}
		}
		*/
		
	}
}

struct GpuRenderer : public Renderer
{
	Color* output = NULL;
	GPUScene sceneGPU;
	
	Random seed;

	GpuRenderer(const Scene* s)
	{
		// upload scene to the GPU
		sceneGPU.sky = s->sky;

		sceneGPU.numPrimitives = s->primitives.size();
		
		if (sceneGPU.numPrimitives > 0)
		{
			cudaMalloc(&sceneGPU.primitives, sizeof(Primitive)*s->primitives.size());
			cudaMemcpy(sceneGPU.primitives, &s->primitives[0], sizeof(Primitive)*s->primitives.size(), cudaMemcpyHostToDevice);
		}

		// build explicit light list
		std::vector<Primitive> lights;
		for (int i=0; i < s->primitives.size(); ++i)
		{
			if (s->primitives[i].light)
				lights.push_back(s->primitives[i]);
		}

		sceneGPU.numLights = lights.size();

		if (sceneGPU.numLights > 0)
		{
			cudaMalloc(&sceneGPU.lights, sizeof(Primitive)*lights.size());
			cudaMemcpy(sceneGPU.lights, &lights[0], sizeof(Primitive)*lights.size(), cudaMemcpyHostToDevice);
		}
	}

	virtual ~GpuRenderer()
	{
		cudaFree(output);
		cudaFree(sceneGPU.primitives);
		cudaFree(sceneGPU.lights);
	}
	
	void Init(int width, int height)
	{
		cudaFree(output);
		cudaMalloc(&output, sizeof(Color)*width*height);
		cudaMemset(output, 0, sizeof(Color)*width*height);
	}

	void Render(Camera* camera, Color* outputHost, int width, int height, int samplesPerPixel, RenderMode mode)
	{
		const int numThreads = width*height;
		const int kNumThreadsPerBlock = 256;
		const int kNumBlocks = (numThreads + kNumThreadsPerBlock - 1) / (kNumThreadsPerBlock);

		const int maxDepth = 4;

		for (int i=0; i < samplesPerPixel; ++i)
		{
			RenderGpu<<<kNumBlocks, kNumThreadsPerBlock>>>(sceneGPU, *camera, width, height, maxDepth, mode, seed.Rand(), output);
		}

		// copy back to output
		cudaMemcpy(outputHost, output, sizeof(Color)*numThreads, cudaMemcpyDeviceToHost);
	}
};


Renderer* CreateGpuRenderer(const Scene* s)
{
	return new GpuRenderer(s);
}
