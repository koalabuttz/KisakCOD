# World front-end test

Asset-free CPU test of the production renderer world front end (scene and view
setup, DPVS portal walk, AABB-tree and cull-group culling, BSP draw-surface
batching, merge) on native i386 Linux, against a synthetic two-cell world.
`r_scene.cpp` and `r_buffers.cpp` are compiled through the native D3D9 boundary,
so the DXVK Native 2.7.1 headers are needed (no DXVK library).

```sh
python3 scripts/tests/run_world_frontend_checks.py --config Debug --dxvk-source <dxvk-2.7.1-checkout>
python3 scripts/tests/run_world_frontend_checks.py --config RelWithDebInfo --dxvk-source <dxvk-2.7.1-checkout>
```

- `world_frontend_test.cpp`: the world, the views and the expected results.
- `frontend_link_boundary.cpp`: everything the linked production units reach
  that the test does not link: host services, zero-initialized storage, and
  traps that abort with their name.

What is covered, how the link is kept honest and what a pass does not claim:
[docs/linux-world-frontend.md](../../docs/linux-world-frontend.md).
