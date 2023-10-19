#pragma once

#include <scene/SceneNodeLookup.h>
#include <render_engine/window/Window.h>

namespace Scene
{
	class SceneRenderManager
	{
	public:
		SceneRenderManager(const SceneNodeLookup& scene_lookup,
			RenderEngine::Window& window)
			: _scene_lookup(scene_lookup)
			, _window(window)
		{}

		void registerMeshesForRender();
	private:
		const SceneNodeLookup& _scene_lookup;
		RenderEngine::Window& _window;
	};
}