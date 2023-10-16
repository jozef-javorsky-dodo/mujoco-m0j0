// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mujoco/mjplugin.h>
#include <mujoco/mjtnum.h>
#include <mujoco/mujoco.h>
#include "elasticity.h"
#include "solid.h"


namespace mujoco::plugin::elasticity {
namespace {

// local tetrahedron numbering
constexpr int kNumEdges = Stencil3D::kNumEdges;
constexpr int kNumVerts = Stencil3D::kNumVerts;
constexpr int edge[kNumEdges][2] = {{0, 1}, {1, 2}, {2, 0},
                                    {2, 3}, {0, 3}, {1, 3}};
constexpr int face[kNumVerts][3] = {{2, 1, 0}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
constexpr int e2f[kNumEdges][2] = {{2, 3}, {1, 3}, {2, 1},
                                   {1, 0}, {0, 2}, {0, 3}};

// volume of a tetrahedron
mjtNum ComputeVolume(const mjtNum* x, const int v[kNumVerts]) {
  mjtNum normal[3];
  mjtNum edge1[3];
  mjtNum edge2[3];
  mjtNum edge3[3];

  mju_sub3(edge1, x+3*v[1], x+3*v[0]);
  mju_sub3(edge2, x+3*v[2], x+3*v[0]);
  mju_sub3(edge3, x+3*v[3], x+3*v[0]);
  mju_cross(normal, edge2, edge1);

  return mju_dot3(normal, edge3) / 6;
}

// compute local basis
void ComputeBasis(mjtNum basis[9], const mjtNum* x, const int v[kNumVerts],
                  const int faceL[3], const int faceR[3], mjtNum volume) {
  mjtNum normalL[3], normalR[3];
  mjtNum edgesL[6], edgesR[6];

  mju_sub3(edgesL+0, x+3*v[faceL[1]], x+3*v[faceL[0]]);
  mju_sub3(edgesL+3, x+3*v[faceL[2]], x+3*v[faceL[0]]);
  mju_sub3(edgesR+0, x+3*v[faceR[1]], x+3*v[faceR[0]]);
  mju_sub3(edgesR+3, x+3*v[faceR[2]], x+3*v[faceR[0]]);

  mju_cross(normalL, edgesL, edgesL+3);
  mju_cross(normalR, edgesR, edgesR+3);

  // we use as basis the symmetrized tensor products of the area normals of the
  // two faces not adjacent to the edge; this is the 3D equivalent to the basis
  // proposed in Weischedel "A discrete geometric view on shear-deformable shell
  // models" in the remark at the end of section 4.1. This is also equivalent to
  // linear finite elements but in a coordinate-free formulation.

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      basis[3*i+j] = ( normalL[i]*normalR[j] +
                       normalR[i]*normalL[j] ) / (36*2*volume*volume);
    }
  }
}

// gradients of edge lengths with respect to vertex positions
void GradSquaredLengths(mjtNum gradient[kNumEdges][2][3],
                        const mjtNum* x,
                        const int v[kNumVerts],
                        const int edge[kNumEdges][2]) {
  for (int e = 0; e < kNumEdges; e++) {
    for (int d = 0; d < 3; d++) {
      gradient[e][0][d] = x[3*v[edge[e][0]]+d] - x[3*v[edge[e][1]]+d];
      gradient[e][1][d] = x[3*v[edge[e][1]]+d] - x[3*v[edge[e][0]]+d];
    }
  }
}

}  // namespace

// factory function
std::optional<Solid> Solid::Create(const mjModel* m, mjData* d, int instance) {
    if (CheckAttr("face", m, instance) &&
        CheckAttr("edge", m, instance) &&
        CheckAttr("poisson", m, instance) &&
        CheckAttr("young", m, instance)) {
        mjtNum nu = strtod(mj_getPluginConfig(m, instance, "poisson"), nullptr);
        mjtNum E = strtod(mj_getPluginConfig(m, instance, "young"), nullptr);
        mjtNum damp =
            strtod(mj_getPluginConfig(m, instance, "damping"), nullptr);
        std::vector<int> face, edge;
        String2Vector(mj_getPluginConfig(m, instance, "face"), face);
        String2Vector(mj_getPluginConfig(m, instance, "edge"), edge);
        return Solid(m, d, instance, nu, E, damp, face, edge);
    } else {
        mju_warning("Invalid parameter specification in solid plugin");
        return std::nullopt;
    }
}

// create map from tetrahedra to vertices and edges and from edges to vertices
void Solid::CreateStencils(const std::vector<int>& simplex,
                           const std::vector<int>& edgeidx) {
  // populate stencil
  nt = simplex.size() / kNumVerts;
  elements.resize(nt);
  for (int t = 0; t < nt; t++) {
    for (int v = 0; v < kNumVerts; v++) {
      elements[t].vertices[v] = simplex[kNumVerts*t+v];
    }
  }

  // map from edge vertices to their index in `edges` vector
  std::unordered_map<std::pair<int, int>, int, PairHash> edge_indices;

  // loop over all tetrahedra
  for (int t = 0; t < nt; t++) {
    int* v = elements[t].vertices;

    // compute edges to vertices map for fast computations
    for (int e = 0; e < kNumEdges; e++) {
      auto pair = std::pair(
        std::min(v[edge[e][0]], v[edge[e][1]]),
        std::max(v[edge[e][0]], v[edge[e][1]])
      );

      // if edge is already present in the vector only store its index
      auto [it, inserted] = edge_indices.insert({pair, ne});

      if (inserted) {
        edges.push_back(pair);
        elements[t].edges[e] = ne++;
      } else {
        elements[t].edges[e] = it->second;
      }

      if (!edgeidx.empty()) {
        assert(elements[t].edges[e] == edgeidx[kNumEdges*t+e]);
      }
    }
  }
}

// plugin constructor
Solid::Solid(const mjModel* m, mjData* d, int instance, mjtNum nu, mjtNum E,
             mjtNum damp, const std::vector<int>& simplex,
             const std::vector<int>& edgeidx)
    : f0(-1), damping(damp) {
  // count plugin bodies
  nv = ne = 0;
  for (int i = 1; i < m->nbody; i++) {
    if (m->body_plugin[i] == instance) {
      if (!nv++) {
        i0 = i;
      }
    }
  }

  // count flexes
  for (int i = 0; i < m->nflex; i++) {
    if (m->flex_vertbodyid[m->flex_vertadr[i]] == i0) {
      f0 = i;
      break;
    }
  }

  // generate tetrahedra from the vertices
  CreateStencils(simplex, edgeidx);

  // allocate arrays
  metric.assign(kNumEdges*kNumEdges*nt, 0);

  // loop over all tetrahedra
  for (int t = 0; t < nt; t++) {
    int* v = elements[t].vertices;
    for (int i = 0; i < kNumVerts; i++) {
      if (m->body_plugin[i0+v[i]] != instance) {
        mju_error("This body does not have the requested plugin instance");
      }
    }

    // tetrahedron volume
    mjtNum volume = ComputeVolume(m->body_pos+3*i0, v);

    // local geometric quantities
    mjtNum basis[kNumEdges][9] = {{0}, {0}, {0}, {0}, {0}, {0}};
    mjtNum trT[kNumEdges] = {0};
    mjtNum trTT[kNumEdges*kNumEdges] = {0};

    // compute edge basis
    for (int e = 0; e < kNumEdges; e++) {
      ComputeBasis(basis[e], m->body_pos+3*i0, v,
                   face[e2f[e][0]], face[e2f[e][1]], volume);
    }

    // compute first invariant i.e. trace(strain)
    for (int e = 0; e < kNumEdges; e++) {
      for (int i = 0; i < 3; i++) {
        trT[e] += basis[e][4*i];
      }
    }

    // compute second invariant i.e. trace(strain^2)
    for (int ed1 = 0; ed1 < kNumEdges; ed1++) {
      for (int ed2 = 0; ed2 < kNumEdges; ed2++) {
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            trTT[kNumEdges*ed1+ed2] += basis[ed1][3*i+j] * basis[ed2][3*j+i];
          }
        }
      }
    }

    // material parameters
    mjtNum mu = E / (2*(1+nu)) * volume;
    mjtNum la = E*nu / ((1+nu)*(1-2*nu)) * volume;

    // assembly of strain metric tensor
    for (int ed1 = 0; ed1 < kNumEdges; ed1++) {
      for (int ed2 = 0; ed2 < kNumEdges; ed2++) {
        int index = kNumEdges*kNumEdges*t + kNumEdges*ed1 + ed2;
        metric[index] = mu * trTT[kNumEdges*ed1+ed2] + la * trT[ed2]*trT[ed1];
      }
    }
  }

  // allocate array
  reference.assign(ne, 0);
  deformed.assign(ne, 0);
  previous.assign(ne, 0);

  // compute edge lengths at equilibrium
  UpdateSquaredLengths(reference, edges, m->body_pos+3*i0);
  previous = reference;
}

void Solid::Compute(const mjModel* m, mjData* d, int instance) {
  // update edges if no flex
  if (f0 < 0) {
    UpdateSquaredLengths(deformed, edges, d->xpos+3*i0);
  }

  // loop over all elements
  for (int t = 0; t < nt; t++)  {
    int* v = elements[t].vertices;

    // compute length gradient with respect to dofs
    mjtNum gradient[kNumEdges][2][3];
    GradSquaredLengths(gradient, d->xpos+3*i0, v, edge);

    // we add generalized Rayleigh damping as decribed in Section 5.2 of
    // Kharevych et al., "Geometric, Variational Integrators for Computer
    // Animation" http://multires.caltech.edu/pubs/DiscreteLagrangian.pdf

    // compute elongation
    mjtNum elongation[kNumEdges];
    for (int e = 0; e < kNumEdges; e++) {
      if (f0 < 0) {
        int idx = elements[t].edges[e];
        mjtNum kD = damping / m->opt.timestep;
        elongation[e] = deformed[idx] - reference[idx] +
                      ( deformed[idx] -  previous[idx] ) * kD;
      } else {
        int idx = elements[t].edges[e] + m->flex_edgeadr[f0];
        mjtNum deformed = d->flexedge_length[idx]*d->flexedge_length[idx];
        mjtNum reference = m->flexedge_length0[idx]*m->flexedge_length0[idx];
        elongation[e] = deformed - reference;
      }
    }

    // we now multiply the elongations by the precomputed metric tensor,
    // notice that if metric=diag(1/reference) then this would yield a
    // mass-spring model

    // compute local force
    mjtNum force[kNumVerts*3] = {0};
    int offset = kNumEdges*kNumEdges;
    for (int ed1 = 0; ed1 < kNumEdges; ed1++) {
      for (int ed2 = 0; ed2 < kNumEdges; ed2++) {
        for (int i = 0; i < 2; i++) {
          for (int x = 0; x < 3; x++) {
            force[3 * edge[ed2][i] + x] +=
                elongation[ed1] * gradient[ed2][i][x] *
                metric[offset * t + kNumEdges * ed1 + ed2];
          }
        }
      }
    }

    // insert into global force
    for (int i = 0; i < kNumVerts; i++) {
      for (int x = 0; x < 3; x++) {
        d->qfrc_passive[m->body_dofadr[i0]+3*v[i]+x] -= force[3*i+x];
      }
    }
  }

  // update stored lengths
  if (f0 < 0) {
    previous = deformed;
  }
}



void Solid::RegisterPlugin() {
  mjpPlugin plugin;
  mjp_defaultPlugin(&plugin);

  plugin.name = "mujoco.elasticity.solid";
  plugin.capabilityflags |= mjPLUGIN_PASSIVE;

  const char* attributes[] = {"face", "edge", "young", "poisson", "damping"};
  plugin.nattribute = sizeof(attributes) / sizeof(attributes[0]);
  plugin.attributes = attributes;
  plugin.nstate = +[](const mjModel* m, int instance) { return 0; };

  plugin.init = +[](const mjModel* m, mjData* d, int instance) {
    auto elasticity_or_null = Solid::Create(m, d, instance);
    if (!elasticity_or_null.has_value()) {
      return -1;
    }
    d->plugin_data[instance] = reinterpret_cast<uintptr_t>(
        new Solid(std::move(*elasticity_or_null)));
    return 0;
  };
  plugin.destroy = +[](mjData* d, int instance) {
    delete reinterpret_cast<Solid*>(d->plugin_data[instance]);
    d->plugin_data[instance] = 0;
  };
  plugin.compute =
      +[](const mjModel* m, mjData* d, int instance, int capability_bit) {
        auto* elasticity = reinterpret_cast<Solid*>(d->plugin_data[instance]);
        elasticity->Compute(m, d, instance);
      };

  mjp_registerPlugin(&plugin);
}

}  // namespace mujoco::plugin::elasticity
