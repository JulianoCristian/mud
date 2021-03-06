//  Copyright (c) 2018 Hugo Amiard hugo.amiard@laposte.net
//  This software is provided 'as-is' under the zlib License, see the LICENSE.txt file.
//  This notice and the license may not be removed or altered from any source distribution.

#include <gfx-pbr/Types.h>
#include <gfx/Cpp20.h>

#include <bgfx/bgfx.h>

#ifdef MUD_MODULES
module mud.gfx.pbr;
#else
#include <pool/ObjectPool.h>
#include <gfx/ManualRender.h>
#include <gfx/Shot.h>
#include <gfx/Graph.h>
#include <gfx/Scene.h>
#include <gfx/Assets.h>
#include <gfx/GfxSystem.h>
#include <gfx-pbr/VoxelGI.h>
#include <gfx-pbr/Light.h>
#endif

namespace mud
{
namespace gfx
{
	template <class T_Element, class... T_Args>
	inline T_Element& create(Scene& scene, T_Args&&... args)
	{
		return scene.m_pool->pool<T_Element>().construct(std::forward<T_Args>(args)...);
	}

	GIProbe& gi_probe(Gnode& parent, uint16_t subdiv, const vec3& extents)
	{
		Gnode& self = parent.suba();
		if(!self.m_gi_probe)
			self.m_gi_probe = &create<GIProbe>(*self.m_scene, *self.m_attach);
		if(subdiv != self.m_gi_probe->m_subdiv || extents != self.m_gi_probe->m_extents)
			self.m_gi_probe->resize(subdiv, extents);
		return *self.m_gi_probe;
	}
}

	GIProbe::GIProbe(Node3& node)
		: m_node(node)
	{}

	void GIProbe::resize(uint16_t subdiv, const vec3& extents)
	{
		m_transform = bxidentity();
		m_subdiv = subdiv;
		m_extents = extents;

		m_raster = bgfx::createTexture2D(subdiv, subdiv, false, 0, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
		m_voxels_color   = bgfx::createTexture3D(subdiv, subdiv, subdiv, false, bgfx::TextureFormat::R32U, BGFX_TEXTURE_COMPUTE_WRITE);
		m_voxels_normals = bgfx::createTexture3D(subdiv, subdiv, subdiv, false, bgfx::TextureFormat::R32U, BGFX_TEXTURE_COMPUTE_WRITE);
		m_voxels_light   = bgfx::createTexture3D(subdiv, subdiv, subdiv, false, bgfx::TextureFormat::R32U, BGFX_TEXTURE_COMPUTE_WRITE);

		bgfx::TextureHandle textures[3] = { m_raster, m_voxels_color, m_voxels_normals };
		m_fbo = bgfx::createFrameBuffer(3, textures);
	}

	PassGIBake::PassGIBake(GfxSystem& gfx_system, BlockGIBake& block_gi_bake)
		: DrawPass(gfx_system, "voxelGI", PassType::VoxelGI)
		, m_block_gi_bake(block_gi_bake)
		, m_voxelize(gfx_system.programs().fetch("gi/voxelize"))
		, m_voxelize_material(&gfx_system.fetch_material("voxelizeGI", "gi/voxelize"))
	{}

	void PassGIBake::next_draw_pass(Render& render, Pass& render_pass)
	{
		UNUSED(render); UNUSED(render_pass);
	}

	void PassGIBake::queue_draw_element(Render& render, DrawElement& element)
	{
		UNUSED(render);
		if(element.m_material->m_pbr_block.m_enabled)
		{
			element.m_material = m_voxelize_material;

			element.m_shader_version = { element.m_material->m_program };

			add_element(element);
		}
	}

	void PassGIBake::submit_draw_element(Pass& render_pass, DrawElement& element) const
	{
		UNUSED(element);

		bgfx::Encoder& encoder = *render_pass.m_encoder;

		GIProbe& gi_probe = *m_block_gi_bake.m_bake_probe;

		int voxels_albedo_index = 1;
		int voxels_normals_index = 2;
		encoder.setUniform(m_block_gi_bake.u_voxelgi.s_voxels_albedo, &voxels_albedo_index);
		encoder.setUniform(m_block_gi_bake.u_voxelgi.s_voxels_normals, &voxels_normals_index);

		m_block_gi_bake.u_voxelgi.setUniforms(encoder, gi_probe);
	}
	
	PassGIProbes::PassGIProbes(GfxSystem& gfx_system, BlockLight& block_light, BlockGIBake& block_gi_bake)
		: RenderPass(gfx_system, "voxelGI", PassType::VoxelGI)
		, m_block_light(block_light)
		, m_block_gi_bake(block_gi_bake)
		, m_voxel_light(gfx_system.programs().fetch("gi/voxel_light"))
	{}

	void PassGIProbes::begin_render_pass(Render& render)
	{
		UNUSED(render);
	}

	void PassGIProbes::submit_render_pass(Render& render)
	{
		if(!check_lighting(render.m_lighting, Lighting::VoxelGI))
			return;

		for(size_t i = 0; i < render.m_shot->m_gi_probes.size(); ++i)
		{
			GIProbe& gi_probe = *render.m_shot->m_gi_probes[i];

			if(gi_probe.m_enabled)
			{
				m_block_gi_bake.m_bake_probe = &gi_probe;

				uvec4 viewport = { uvec2(0), uvec2(gi_probe.m_subdiv) };
				ManualRender voxel_render = { render, gi_probe.m_fbo, viewport,  gi_probe.m_transform, bxidentity() };
				Renderer& renderer = m_gfx_system.renderer(Shading::Voxels);

				voxel_render.cull();
				voxel_render.render(renderer);

				Pass render_pass = render.next_pass("compute light");
				bgfx::Encoder& encoder = *render_pass.m_encoder;

				//encoder.setImage(0, gi_probe.m_voxels_color,   0, bgfx::Access::Read,      bgfx::TextureFormat::RGBA8);
				encoder.setImage(0, gi_probe.m_voxels_color, 0, bgfx::Access::Read, bgfx::TextureFormat::R32U);
				encoder.setImage(1, gi_probe.m_voxels_normals, 0, bgfx::Access::Read, bgfx::TextureFormat::R32U);
				encoder.setImage(2, gi_probe.m_voxels_light, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::R32U);

				int voxels_albedo_index = 0;
				int voxels_normals_index = 1;
				int voxels_light_index = 2;
				encoder.setUniform(m_block_gi_bake.u_voxelgi.s_voxels_albedo, &voxels_albedo_index);
				encoder.setUniform(m_block_gi_bake.u_voxelgi.s_voxels_normals, &voxels_normals_index);
				encoder.setUniform(m_block_gi_bake.u_voxelgi.s_voxels_light, &voxels_light_index);

				m_block_gi_bake.u_voxelgi.setUniforms(encoder, gi_probe);

				m_block_light.upload_lights(render, render_pass);

				ShaderVersion shader_version = { &m_voxel_light };
				if(m_block_light.m_directional_light)
					shader_version.set_option(m_block_light.m_index, DIRECTIONAL_LIGHT, true);

				uint16_t subdiv = gi_probe.m_subdiv;
				bgfx::ProgramHandle program = m_voxel_light.version(shader_version);
				if(bgfx::isValid(program))
					encoder.dispatch(render_pass.m_index, program, subdiv / 64, subdiv, subdiv, BGFX_VIEW_NONE);
				else
					printf("WARNING: invalid voxel light program\n");
			}
		}
	}

	BlockGIBake::BlockGIBake(GfxSystem& gfx_system)
		: GfxBlock(gfx_system, type<BlockGIBake>())
	{}

	void BlockGIBake::init_gfx_block()
	{
		u_voxelgi.createUniforms();
	}

	void BlockGIBake::begin_gfx_block(Render& render)
	{
		UNUSED(render);
	}

	void BlockGIBake::submit_gfx_block(Render& render)
	{
		UNUSED(render);
	}

	BlockGITrace::BlockGITrace(GfxSystem& gfx_system)
		: DrawBlock(gfx_system, type<BlockGITrace>())
	{
		static cstring options[1] = { "GI_CONETRACE" };
		m_shader_block->m_options = { options, 1 };
	}

	void BlockGITrace::init_gfx_block()
	{
		u_gi_probe.createUniforms();
	}

	void BlockGITrace::begin_gfx_block(Render& render)
	{
		UNUSED(render);
	}

	void BlockGITrace::submit_gfx_block(Render& render)
	{
		UNUSED(render);
	}

	void BlockGITrace::begin_gfx_pass(Render& render)
	{
		UNUSED(render);
	}

	void BlockGITrace::submit_gfx_element(Render& render, const Pass& render_pass, DrawElement& element) const
	{
		this->submit_pass(render, render_pass, element.m_shader_version);
	}

	void BlockGITrace::submit_gfx_cluster(Render& render, const Pass& render_pass, DrawCluster& cluster) const
	{
		this->submit_pass(render, render_pass, cluster.m_shader_version);
	}

	void BlockGITrace::submit_pass(Render& render, const Pass& render_pass, ShaderVersion& shader_version) const
	{
		UNUSED(render); UNUSED(render_pass);

		bgfx::Encoder& encoder = *render_pass.m_encoder;

		uint8_t index = 0;
		for(GIProbe* gi_probe : render.m_shot->m_gi_probes)
		{
			if(gi_probe->m_enabled)
			{
				float multiplier = gi_probe->m_dynamic_range * gi_probe->m_energy;
				vec4 params = { multiplier, gi_probe->m_bias, gi_probe->m_normal_bias, float(gi_probe->m_interior) };
				mat4 transform = gi_probe->m_transform * inverse(render.m_camera.m_transform);
				vec4 cell_size = { Unit3 / vec3(float(gi_probe->m_subdiv)), 1.f };

				encoder.setTexture(uint8_t(TextureSampler::GIProbe) + index++, u_gi_probe.s_gi_probe, gi_probe->m_voxels_light);

				encoder.setUniform(u_gi_probe.u_transform, &transform);
				encoder.setUniform(u_gi_probe.u_bounds, &gi_probe->m_extents);
				encoder.setUniform(u_gi_probe.u_params, &params);
				encoder.setUniform(u_gi_probe.u_cell_size, &cell_size);

				//shader_version.set_option(m_index, GI_CONETRACE, true);
			}
		}
	}

	void BlockGITrace::upload_gi_probes(Render& render, const Pass& render_pass) const
	{
		UNUSED(render); UNUSED(render_pass);

		//bgfx::Encoder& encoder = *render_pass.m_encoder;
	}
}
