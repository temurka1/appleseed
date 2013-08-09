
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "drtlightingengine.h"

// appleseed.renderer headers.
#include "renderer/kernel/aov/spectrumstack.h"
#include "renderer/kernel/lighting/directlightingintegrator.h"
#include "renderer/kernel/lighting/imagebasedlighting.h"
#include "renderer/kernel/lighting/lightsampler.h"
#include "renderer/kernel/lighting/pathtracer.h"
#include "renderer/kernel/shading/shadingcontext.h"
#include "renderer/kernel/shading/shadingpoint.h"
#include "renderer/modeling/bsdf/bsdf.h"
#include "renderer/modeling/edf/edf.h"
#include "renderer/modeling/environment/environment.h"
#include "renderer/modeling/environmentedf/environmentedf.h"
#include "renderer/modeling/input/inputevaluator.h"
#include "renderer/modeling/material/material.h"
#include "renderer/modeling/scene/scene.h"

// appleseed.foundation headers.
#include "foundation/math/basis.h"
#include "foundation/math/mis.h"
#include "foundation/math/population.h"
#include "foundation/utility/memory.h"
#include "foundation/utility/statistics.h"
#include "foundation/utility/string.h"

// Forward declarations.
namespace renderer  { class EnvironmentEDF; }

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // Distribution Ray Tracing (DRT) lighting engine.
    //

    class DRTLightingEngine
      : public ILightingEngine
    {
      public:
        struct Parameters
        {
            const bool      m_enable_ibl;                   // image-based lighting enabled?

            const size_t    m_max_path_length;              // maximum path length, ~0 for unlimited
            const size_t    m_rr_min_path_length;           // minimum path length before Russian Roulette kicks in, ~0 for unlimited

            const size_t    m_dl_light_sample_count;        // number of light samples used to estimate direct illumination
            const size_t    m_ibl_env_sample_count;         // number of environment samples used to estimate IBL

            explicit Parameters(const ParamArray& params)
              : m_enable_ibl(params.get_optional<bool>("enable_ibl", true))
              , m_max_path_length(nz(params.get_optional<size_t>("max_path_length", 0)))
              , m_rr_min_path_length(nz(params.get_optional<size_t>("rr_min_path_length", 3)))
              , m_dl_light_sample_count(params.get_optional<size_t>("dl_light_samples", 1))
              , m_ibl_env_sample_count(params.get_optional<size_t>("ibl_env_samples", 1))
            {
            }

            static size_t nz(const size_t x)
            {
                return x == 0 ? ~0 : x;
            }

            void print() const
            {
                RENDERER_LOG_INFO(
                    "distribution ray tracing settings:\n"
                    "  ibl              %s\n"
                    "  max path length  %s\n"
                    "  rr min path len. %s\n"
                    "  dl light samples %s\n"
                    "  ibl env samples  %s",
                    m_enable_ibl ? "on" : "off",
                    m_max_path_length == ~0 ? "infinite" : pretty_uint(m_max_path_length).c_str(),
                    m_rr_min_path_length == ~0 ? "infinite" : pretty_uint(m_rr_min_path_length).c_str(),
                    pretty_uint(m_dl_light_sample_count).c_str(),
                    pretty_uint(m_ibl_env_sample_count).c_str());
            }
        };

        DRTLightingEngine(
            const LightSampler&     light_sampler,
            const ParamArray&       params)
          : m_params(params)
          , m_light_sampler(light_sampler)
          , m_path_count(0)
        {
        }

        virtual void release() OVERRIDE
        {
            delete this;
        }

        virtual void compute_lighting(
            SamplingContext&        sampling_context,
            const ShadingContext&   shading_context,
            const ShadingPoint&     shading_point,
            Spectrum&               radiance,               // output radiance, in W.sr^-1.m^-2
            SpectrumStack&          aovs) OVERRIDE
        {
            PathVisitor path_visitor(
                m_params,
                m_light_sampler,
                shading_context,
                shading_point.get_scene(),
                radiance,
                aovs);

            PathTracer<PathVisitor, false> path_tracer(     // false = not adjoint
                path_visitor,
                m_params.m_rr_min_path_length,
                m_params.m_max_path_length,
                shading_context.get_max_iterations());

            const size_t path_length =
                path_tracer.trace(
                    sampling_context,
                    shading_context.get_intersector(),
                    shading_context.get_texture_cache(),
                    shading_point);

            // Update statistics.
            ++m_path_count;
            m_path_length.insert(path_length);
        }

        virtual StatisticsVector get_statistics() const OVERRIDE
        {
            Statistics stats;
            stats.insert("path count", m_path_count);
            stats.insert("path length", m_path_length);

            return StatisticsVector::make("distribution ray tracing statistics", stats);
        }

      private:
        const Parameters        m_params;
        const LightSampler&     m_light_sampler;

        uint64                  m_path_count;
        Population<uint64>      m_path_length;

        struct PathVisitor
        {
            const Parameters&           m_params;
            const LightSampler&         m_light_sampler;
            const ShadingContext&       m_shading_context;
            TextureCache&               m_texture_cache;
            const EnvironmentEDF*       m_env_edf;
            Spectrum&                   m_path_radiance;
            SpectrumStack&              m_path_aovs;

            PathVisitor(
                const Parameters&       params,
                const LightSampler&     light_sampler,
                const ShadingContext&   shading_context,
                const Scene&            scene,
                Spectrum&               path_radiance,
                SpectrumStack&          path_aovs)
              : m_params(params)
              , m_light_sampler(light_sampler)
              , m_shading_context(shading_context)
              , m_texture_cache(shading_context.get_texture_cache())
              , m_env_edf(scene.get_environment()->get_environment_edf())
              , m_path_radiance(path_radiance)
              , m_path_aovs(path_aovs)
            {
            }

            bool accept_scattering_mode(
                const BSDF::Mode        prev_bsdf_mode,
                const BSDF::Mode        bsdf_mode) const
            {
                assert(bsdf_mode != BSDF::Absorption);

                // No diffuse bounces.
                if (BSDF::has_diffuse(bsdf_mode))
                    return false;

                // No caustics.
                if (BSDF::has_diffuse(prev_bsdf_mode) && BSDF::has_glossy_or_specular(bsdf_mode))
                    return false;

                return true;
            }

            bool visit_vertex(const PathVertex& vertex)
            {
                const int light_sampling_modes =
                    vertex.m_prev_bsdf_mode == BSDF::Diffuse
                        ? BSDF::Diffuse
                        : BSDF::AllScatteringModes;

                Spectrum vertex_radiance;
                SpectrumStack vertex_aovs(m_path_aovs.size());

                if (vertex.m_bsdf)
                {
                    add_direct_lighting_contribution(
                        vertex,
                        light_sampling_modes,
                        vertex_radiance,
                        vertex_aovs);

                    if (m_env_edf && m_params.m_enable_ibl)
                    {
                        add_image_based_lighting_contribution(
                            vertex,
                            light_sampling_modes,
                            vertex_radiance,
                            vertex_aovs);
                    }
                }
                else
                {
                    vertex_radiance.set(0.0f);
                    vertex_aovs.set(0.0f);
                }

                add_emitted_light_contribution(
                    vertex,
                    vertex_radiance,
                    vertex_aovs);

                // Update the path radiance.
                vertex_radiance *= vertex.m_throughput;
                m_path_radiance += vertex_radiance;
                vertex_aovs *= vertex.m_throughput;
                m_path_aovs += vertex_aovs;

                // Proceed with this path.
                return true;
            }

            void add_direct_lighting_contribution(
                const PathVertex&       vertex,
                const int               light_sampling_modes,
                Spectrum&               vertex_radiance,
                SpectrumStack&          vertex_aovs)
            {
                // We compute direct lighting by sampling both the lights and the BSDF.
                // Unlike in the path tracer, we need to sample the diffuse components
                // of the BSDF (with a single sample) because we won't extend the path
                // after a diffuse bounce.
                DirectLightingIntegrator integrator(
                    m_shading_context,
                    m_light_sampler,
                    vertex,
                    BSDF::Diffuse,
                    light_sampling_modes,
                    1,                                      // a single BSDF sample since the path will be extended with a single ray
                    m_params.m_dl_light_sample_count);      // the number of light samples is user-adjustable
                integrator.sample_bsdf_and_lights_low_variance(
                    false,                                  // never in an indirect lighting situation
                    vertex.m_sampling_context,
                    vertex_radiance,
                    vertex_aovs);
            }

            void add_image_based_lighting_contribution(
                const PathVertex&       vertex,
                const int               light_sampling_modes,
                Spectrum&               vertex_radiance,
                SpectrumStack&          vertex_aovs)
            {
                Spectrum ibl_radiance;

                // We compute image-based lighting by sampling both the environment and
                // the BSDF. When sampling the BSDF, we limit ourselves to sampling
                // diffuse components of the BSDF (with a single sample) since sampling
                // of glossy components will be done by extending the current path.
                compute_ibl(
                    m_shading_context,
                    *m_env_edf,
                    vertex,
                    BSDF::Diffuse,
                    light_sampling_modes,
                    1,                                      // a single BSDF sample since the path will be extended with a single ray
                    m_params.m_ibl_env_sample_count,        // the number of environment samples is user-adjustable
                    ibl_radiance);

                vertex_radiance += ibl_radiance;
                vertex_aovs.add(m_env_edf->get_render_layer_index(), ibl_radiance);
            }

            void add_emitted_light_contribution(
                const PathVertex&       vertex,
                Spectrum&               vertex_radiance,
                SpectrumStack&          vertex_aovs)
            {
                if (vertex.m_edf && vertex.m_cos_on > 0.0)
                {
                    // Compute the emitted radiance.
                    Spectrum emitted_radiance;
                    vertex.compute_emitted_radiance(m_texture_cache, emitted_radiance);

                    // Multiple importance sampling.
                    if (vertex.m_prev_bsdf_mode != BSDF::Specular)
                    {
                        const double mis_weight =
                            mis_power2(
                                1 * vertex.get_bsdf_point_prob(),
                                m_params.m_dl_light_sample_count * vertex.get_light_point_prob(m_light_sampler));
                        emitted_radiance *= static_cast<float>(mis_weight);
                    }

                    vertex_radiance += emitted_radiance;
                    vertex_aovs.add(vertex.m_edf->get_render_layer_index(), emitted_radiance);
                }
            }

            void visit_environment(
                const ShadingPoint&     shading_point,
                const Vector3d&         outgoing,
                const BSDF::Mode        prev_bsdf_mode,
                const double            prev_bsdf_prob,
                const Spectrum&         throughput)
            {
                assert(prev_bsdf_mode != BSDF::Absorption);

                // Can't look up the environment if there's no environment EDF.
                if (m_env_edf == 0)
                    return;

                // When IBL is disabled, only specular reflections should contribute here.
                if (!m_params.m_enable_ibl && prev_bsdf_mode != BSDF::Specular)
                    return;

                // Evaluate the environment EDF.
                InputEvaluator input_evaluator(m_texture_cache);
                Spectrum env_radiance;
                double env_prob;
                m_env_edf->evaluate(
                    input_evaluator,
                    -outgoing,
                    env_radiance,
                    env_prob);

                // Multiple importance sampling.
                if (prev_bsdf_mode != BSDF::Specular)
                {
                    assert(prev_bsdf_prob > 0.0);
                    const double mis_weight =
                        mis_power2(
                            1 * prev_bsdf_prob,
                            m_params.m_ibl_env_sample_count * env_prob);
                    env_radiance *= static_cast<float>(mis_weight);
                }

                // Update the path radiance.
                env_radiance *= throughput;
                m_path_radiance += env_radiance;
                m_path_aovs.add(m_env_edf->get_render_layer_index(), env_radiance);
            }
        };
    };
}


//
// DRTLightingEngineFactory class implementation.
//

DRTLightingEngineFactory::DRTLightingEngineFactory(
    const LightSampler& light_sampler,
    const ParamArray&   params)
  : m_light_sampler(light_sampler)
  , m_params(params)
{
    DRTLightingEngine::Parameters(params).print();
}

void DRTLightingEngineFactory::release()
{
    delete this;
}

ILightingEngine* DRTLightingEngineFactory::create()
{
    return new DRTLightingEngine(m_light_sampler, m_params);
}

}   // namespace renderer
