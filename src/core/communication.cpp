/*
  Copyright (C) 2010-2018 The ESPResSo project
  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
    Max-Planck-Institute for Polymer Research, Theory Group

  This file is part of ESPResSo.

  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mpi.h>
#ifdef OPEN_MPI
#include <dlfcn.h>
#endif
#include <cassert>

#include "communication.hpp"

#include "errorhandling.hpp"

#include "EspressoSystemInterface.hpp"
#include "bonded_interactions/bonded_tab.hpp"
#include "cells.hpp"
#include "collision.hpp"
#include "cuda_interface.hpp"
#include "energy.hpp"
#include "event.hpp"
#include "forces.hpp"
#include "galilei.hpp"
#include "global.hpp"
#include "grid.hpp"
#include "grid_based_algorithms/lb.hpp"
#include "grid_based_algorithms/lb_interface.hpp"
#include "grid_based_algorithms/lb_interpolation.hpp"
#include "grid_based_algorithms/lb_particle_coupling.hpp"
#include "integrate.hpp"
#include "io/mpiio/mpiio.hpp"
#include "minimize_energy.hpp"
#include "nonbonded_interactions/nonbonded_tab.hpp"
#include "npt.hpp"
#include "partCfg_global.hpp"
#include "particle_data.hpp"
#include "pressure.hpp"
#include "rotation.hpp"
#include "statistics.hpp"
#include "statistics_chain.hpp"
#include "swimmer_reaction.hpp"
#include "virtual_sites.hpp"

#include "electrostatics_magnetostatics/coulomb.hpp"
#include "electrostatics_magnetostatics/dipole.hpp"
#include "electrostatics_magnetostatics/icc.hpp"
#include "electrostatics_magnetostatics/mdlc_correction.hpp"

#include "serialization/IA_parameters.hpp"
#include "serialization/Particle.hpp"
#include "serialization/ParticleParametersSwimming.hpp"

#include "utils/u32_to_u64.hpp"
#include <utils/Counter.hpp>

#include <boost/mpi.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/string.hpp>

using namespace std;

namespace Communication {
auto const &mpi_datatype_cache = boost::mpi::detail::mpi_datatype_cache();
std::unique_ptr<boost::mpi::environment> mpi_env;
} // namespace Communication

boost::mpi::communicator comm_cart;

namespace Communication {
std::unique_ptr<MpiCallbacks> m_callbacks;

/* We use a singleton callback class for now. */
MpiCallbacks &mpiCallbacks() {
  assert(m_callbacks && "Mpi not initialized!");

  return *m_callbacks;
}
} // namespace Communication

using Communication::mpiCallbacks;

int this_node = -1;
int n_nodes = -1;

// if you want to add a callback, add it here, and here only
#define CALLBACK_LIST                                                          \
  CB(mpi_who_has_slave)                                                        \
  CB(mpi_place_particle_slave)                                                 \
  CB(mpi_recv_part_slave)                                                      \
  CB(mpi_integrate_slave)                                                      \
  CB(mpi_bcast_ia_params_slave)                                                \
  CB(mpi_bcast_all_ia_params_slave)                                            \
  CB(mpi_bcast_max_seen_particle_type_slave)                                   \
  CB(mpi_gather_stats_slave)                                                   \
  CB(mpi_bcast_coulomb_params_slave)                                           \
  CB(mpi_place_new_particle_slave)                                             \
  CB(mpi_remove_particle_slave)                                                \
  CB(mpi_rescale_particles_slave)                                              \
  CB(mpi_bcast_cell_structure_slave)                                           \
  CB(mpi_bcast_nptiso_geom_slave)                                              \
  CB(mpi_bcast_cuda_global_part_vars_slave)                                    \
  CB(mpi_bcast_max_mu_slave)                                                   \
  CB(mpi_kill_particle_motion_slave)                                           \
  CB(mpi_kill_particle_forces_slave)                                           \
  CB(mpi_system_CMS_slave)                                                     \
  CB(mpi_system_CMS_velocity_slave)                                            \
  CB(mpi_galilei_transform_slave)                                              \
  CB(mpi_setup_reaction_slave)                                                 \
  CB(mpi_check_runtime_errors_slave)                                           \
  CB(mpi_minimize_energy_slave)                                                \
  CB(mpi_gather_cuda_devices_slave)                                            \
  CB(mpi_resort_particles_slave)                                               \
  CB(mpi_get_pairs_slave)                                                      \
  CB(mpi_get_particles_slave)                                                  \
  CB(mpi_rotate_system_slave)                                                  \
  CB(mpi_update_particle_slave)                                                \
  CB(mpi_bcast_lb_particle_coupling_slave)                                     \
  CB(mpi_recv_lb_interpolated_velocity_slave)                                  \
  CB(mpi_set_interpolation_order_slave)

// create the forward declarations
#define CB(name) void name(int node, int param);
CALLBACK_LIST

#undef CB

#ifdef DOXYGEN
    (void); /* this line prevents an interaction in Doxygen between
               CALLBACK_LIST and the anonymous namespace that follows */
#endif

namespace {
#ifdef COMM_DEBUG
// create the list of names
#define CB(name) #name,

/** List of callback names for debugging. */
std::vector<std::string> names{CALLBACK_LIST};
#undef CB
#endif
} // namespace

/** Forward declarations */

int mpi_check_runtime_errors();

/**********************************************
 * procedures
 **********************************************/

#if defined(OPEN_MPI)
/** Workaround for "Read -1, expected XXXXXXX, errno = 14" that sometimes
 *  appears when CUDA is used. This is a bug in OpenMPI 2.0-2.1.2 and 3.0.0
 *  according to
 *  https://www.mail-archive.com/users@lists.open-mpi.org/msg32357.html,
 *  so we set btl_vader_single_copy_mechanism = none.
 */
static void openmpi_fix_vader() {
  if (OMPI_MAJOR_VERSION < 2 || OMPI_MAJOR_VERSION > 3)
    return;
  if (OMPI_MAJOR_VERSION == 2 && OMPI_MINOR_VERSION == 1 &&
      OMPI_RELEASE_VERSION >= 3)
    return;
  if (OMPI_MAJOR_VERSION == 3 &&
      (OMPI_MINOR_VERSION > 0 || OMPI_RELEASE_VERSION > 0))
    return;

  std::string varname = "btl_vader_single_copy_mechanism";
  std::string varval = "none";

  setenv((std::string("OMPI_MCA_") + varname).c_str(), varval.c_str(), 0);
}
#endif

void mpi_init() {
#ifdef OPEN_MPI
  openmpi_fix_vader();

  void *handle = nullptr;
  int mode = RTLD_NOW | RTLD_GLOBAL;
#ifdef RTLD_NOLOAD
  mode |= RTLD_NOLOAD;
#endif
  void *_openmpi_symbol = dlsym(RTLD_DEFAULT, "MPI_Init");
  if (!_openmpi_symbol) {
    fprintf(stderr, "%d: Aborting because unable to find OpenMPI symbol.\n",
            this_node);
    errexit();
  }
  Dl_info _openmpi_info;
  dladdr(_openmpi_symbol, &_openmpi_info);

  if (!handle)
    handle = dlopen(_openmpi_info.dli_fname, mode);

  if (!handle) {
    fprintf(stderr,
            "%d: Aborting because unable to load libmpi into the "
            "global symbol space.\n",
            this_node);
    errexit();
  }
#endif

#ifdef BOOST_MPI_HAS_NOARG_INITIALIZATION
  Communication::mpi_env = std::make_unique<boost::mpi::environment>();
#else
  int argc{};
  char **argv{};
  Communication::mpi_env =
      std::make_unique<boost::mpi::environment>(argc, argv);
#endif

  MPI_Comm_size(MPI_COMM_WORLD, &n_nodes);
  MPI_Dims_create(n_nodes, 3, node_grid.data());

  mpi_reshape_communicator({{node_grid[0], node_grid[1], node_grid[2]}},
                           /* periodicity */ {{1, 1, 1}});
  MPI_Cart_coords(comm_cart, this_node, 3, node_pos.data());

  Communication::m_callbacks =
      std::make_unique<Communication::MpiCallbacks>(comm_cart);

#define CB(name) Communication::m_callbacks->add(&name);
  CALLBACK_LIST
#undef CB

  ErrorHandling::init_error_handling(mpiCallbacks());
  partCfg(std::make_unique<PartCfg>(mpiCallbacks(), GetLocalParts()));

  on_program_start();
}

void mpi_reshape_communicator(std::array<int, 3> const &node_grid,
                              std::array<int, 3> const &periodicity) {
  MPI_Comm temp_comm;
  MPI_Cart_create(MPI_COMM_WORLD, 3, const_cast<int *>(node_grid.data()),
                  const_cast<int *>(periodicity.data()), 0, &temp_comm);
  comm_cart =
      boost::mpi::communicator(temp_comm, boost::mpi::comm_take_ownership);

  this_node = comm_cart.rank();
}

void mpi_call(SlaveCallback cb, int node, int param) {
  mpiCallbacks().call(cb, node, param);

  COMM_TRACE(fprintf(stderr, "%d: finished sending.\n", this_node));
}

/****************** REQ_PLACE/REQ_PLACE_NEW ************/

void mpi_place_particle(int pnode, int part, double p[3]) {
  mpi_call(mpi_place_particle_slave, pnode, part);

  if (pnode == this_node)
    local_place_particle(part, p, 0);
  else
    MPI_Send(p, 3, MPI_DOUBLE, pnode, SOME_TAG, comm_cart);

  set_resort_particles(Cells::RESORT_GLOBAL);
  on_particle_change();
}

void mpi_place_particle_slave(int pnode, int part) {

  if (pnode == this_node) {
    double p[3];
    MPI_Recv(p, 3, MPI_DOUBLE, 0, SOME_TAG, comm_cart, MPI_STATUS_IGNORE);
    local_place_particle(part, p, 0);
  }

  set_resort_particles(Cells::RESORT_GLOBAL);
  on_particle_change();
}

void mpi_place_new_particle(int pnode, int part, double p[3]) {
  mpi_call(mpi_place_new_particle_slave, pnode, part);
  added_particle(part);

  if (pnode == this_node)
    local_place_particle(part, p, 1);
  else
    MPI_Send(p, 3, MPI_DOUBLE, pnode, SOME_TAG, comm_cart);

  on_particle_change();
}

void mpi_place_new_particle_slave(int pnode, int part) {

  added_particle(part);

  if (pnode == this_node) {
    double p[3];
    MPI_Recv(p, 3, MPI_DOUBLE, 0, SOME_TAG, comm_cart, MPI_STATUS_IGNORE);
    local_place_particle(part, p, 1);
  }

  on_particle_change();
}

/****************** REQ_GET_PART ************/
Particle mpi_recv_part(int pnode, int part) {
  Particle ret;

  mpi_call(mpi_recv_part_slave, pnode, part);
  comm_cart.recv(pnode, SOME_TAG, ret);

  return ret;
}

void mpi_recv_part_slave(int pnode, int part) {
  if (pnode != this_node)
    return;

  assert(local_particles[part]);
  comm_cart.send(0, SOME_TAG, *local_particles[part]);
}

/****************** REQ_REM_PART ************/
void mpi_remove_particle(int pnode, int part) {
  mpi_call(mpi_remove_particle_slave, pnode, part);
  mpi_remove_particle_slave(pnode, part);
}

void mpi_remove_particle_slave(int pnode, int part) {
  if (part != -1) {
    n_part--;

    if (pnode == this_node)
      local_remove_particle(part);

    remove_all_bonds_to(part);
  } else
    local_remove_all_particles();

  on_particle_change();
}

/********************* REQ_MIN_ENERGY ********/

int mpi_minimize_energy() {
  mpi_call(mpi_minimize_energy_slave, 0, 0);
  return minimize_energy();
}

void mpi_minimize_energy_slave(int, int) { minimize_energy(); }

/********************* REQ_INTEGRATE ********/
int mpi_integrate(int n_steps, int reuse_forces) {
  mpi_call(mpi_integrate_slave, n_steps, reuse_forces);
  integrate_vv(n_steps, reuse_forces);
  COMM_TRACE(
      fprintf(stderr, "%d: integration task %d done.\n", this_node, n_steps));
  return mpi_check_runtime_errors();
}

void mpi_integrate_slave(int n_steps, int reuse_forces) {
  integrate_vv(n_steps, reuse_forces);
  COMM_TRACE(fprintf(
      stderr, "%d: integration for %d n_steps with %d reuse_forces done.\n",
      this_node, n_steps, reuse_forces));
}

/*************** REQ_BCAST_IA ************/
void mpi_bcast_all_ia_params() {
  mpi_call(mpi_bcast_all_ia_params_slave, -1, -1);
  boost::mpi::broadcast(comm_cart, ia_params, 0);
}

void mpi_bcast_all_ia_params_slave(int, int) {
  boost::mpi::broadcast(comm_cart, ia_params, 0);
}

void mpi_bcast_ia_params(int i, int j) {
  mpi_call(mpi_bcast_ia_params_slave, i, j);

  if (j >= 0) {
    /* non-bonded interaction parameters */
    boost::mpi::broadcast(comm_cart, *get_ia_param(i, j), 0);

    *get_ia_param(j, i) = *get_ia_param(i, j);
  } else {
    /* bonded interaction parameters */
    MPI_Bcast(&(bonded_ia_params[i]), sizeof(Bonded_ia_parameters), MPI_BYTE, 0,
              comm_cart);
#ifdef TABULATED
    /* For tabulated potentials we have to send the tables extra */
    if (bonded_ia_params[i].type == BONDED_IA_TABULATED) {
      boost::mpi::broadcast(comm_cart, *bonded_ia_params[i].p.tab.pot, 0);
    }
#endif
  }

  on_short_range_ia_change();
}

void mpi_bcast_ia_params_slave(int i, int j) {
  if (j >= 0) { /* non-bonded interaction parameters */

    boost::mpi::broadcast(comm_cart, *get_ia_param(i, j), 0);

    *get_ia_param(j, i) = *get_ia_param(i, j);

  } else {                   /* bonded interaction parameters */
    make_bond_type_exist(i); /* realloc bonded_ia_params on slave nodes! */
    MPI_Bcast(&(bonded_ia_params[i]), sizeof(Bonded_ia_parameters), MPI_BYTE, 0,
              comm_cart);
#ifdef TABULATED
    /* For tabulated potentials we have to send the tables extra */
    if (bonded_ia_params[i].type == BONDED_IA_TABULATED) {
      auto *tab_pot = new TabulatedPotential();
      boost::mpi::broadcast(comm_cart, *tab_pot, 0);

      bonded_ia_params[i].p.tab.pot = tab_pot;
    }
#endif
  }

  on_short_range_ia_change();
}

/*************** REQ_BCAST_IA_SIZE ************/

void mpi_bcast_max_seen_particle_type(int ns) {
  mpi_call(mpi_bcast_max_seen_particle_type_slave, -1, ns);
  mpi_bcast_max_seen_particle_type_slave(-1, ns);
}

void mpi_bcast_max_seen_particle_type_slave(int, int ns) {
  realloc_ia_params(ns);
}

/*************** REQ_GATHER ************/
void mpi_gather_stats(int job, void *result, void *result_t, void *result_nb,
                      void *result_t_nb) {
  switch (job) {
  case 1:
    mpi_call(mpi_gather_stats_slave, -1, 1);
    energy_calc((double *)result);
    break;
  case 2:
    /* calculate and reduce (sum up) virials for 'analyze pressure' or
       'analyze stress_tensor' */
    mpi_call(mpi_gather_stats_slave, -1, 2);
    pressure_calc((double *)result, (double *)result_t, (double *)result_nb,
                  (double *)result_t_nb, 0);
    break;
  case 3:
    mpi_call(mpi_gather_stats_slave, -1, 3);
    pressure_calc((double *)result, (double *)result_t, (double *)result_nb,
                  (double *)result_t_nb, 1);
    break;
  case 4:
    mpi_call(mpi_gather_stats_slave, -1, 4);
    predict_momentum_particles((double *)result);
    break;
#ifdef LB
  case 6:
    mpi_call(mpi_gather_stats_slave, -1, 6);
    lb_calc_fluid_momentum((double *)result);
    break;
  case 7:
    break;
#ifdef LB_BOUNDARIES
  case 8:
    mpi_call(mpi_gather_stats_slave, -1, 8);
    lb_collect_boundary_forces((double *)result);
    break;
#endif
#endif
  default:
    fprintf(
        stderr,
        "%d: INTERNAL ERROR: illegal request %d for mpi_gather_stats_slave\n",
        this_node, job);
    errexit();
  }
}

void mpi_gather_stats_slave(int, int job) {
  switch (job) {
  case 1:
    /* calculate and reduce (sum up) energies */
    energy_calc(nullptr);
    break;
  case 2:
    /* calculate and reduce (sum up) virials for 'analyze pressure' or 'analyze
     * stress_tensor'*/
    pressure_calc(nullptr, nullptr, nullptr, nullptr, 0);
    break;
  case 3:
    /* calculate and reduce (sum up) virials, revert velocities half a timestep
     * for 'analyze p_inst' */
    pressure_calc(nullptr, nullptr, nullptr, nullptr, 1);
    break;
  case 4:
    predict_momentum_particles(nullptr);
    break;
#ifdef LB
  case 6:
    lb_calc_fluid_momentum(nullptr);
    break;
  case 7:
    break;
#ifdef LB_BOUNDARIES
  case 8:
    lb_collect_boundary_forces(nullptr);
    break;
#endif
#endif
  default:
    fprintf(
        stderr,
        "%d: INTERNAL ERROR: illegal request %d for mpi_gather_stats_slave\n",
        this_node, job);
    errexit();
  }
}

/*************** REQ_SET_TIME_STEP ************/
void mpi_set_time_step_slave(double dt) {
  time_step = dt;
  time_step_squared = time_step * time_step;
  time_step_squared_half = time_step_squared / 2.;
  time_step_half = time_step / 2.;

  on_parameter_change(FIELD_TIMESTEP);
}

REGISTER_CALLBACK(mpi_set_time_step_slave)

void mpi_set_time_step(double time_s) {
  mpiCallbacks().call(mpi_set_time_step_slave, time_s);
  mpi_set_time_step_slave(time_s);
}

int mpi_check_runtime_errors() {
  mpi_call(mpi_check_runtime_errors_slave, 0, 0);
  return check_runtime_errors();
}

void mpi_check_runtime_errors_slave(int, int) { check_runtime_errors(); }

/*************** REQ_BCAST_COULOMB ************/
void mpi_bcast_coulomb_params() {
#if defined(ELECTROSTATICS) || defined(DIPOLES)
  mpi_call(mpi_bcast_coulomb_params_slave, 1, 0);
  mpi_bcast_coulomb_params_slave(-1, 0);
#endif
}

void mpi_bcast_coulomb_params_slave(int, int) {

#if defined(ELECTROSTATICS) || defined(DIPOLES)

#ifdef ELECTROSTATICS
  MPI_Bcast(&coulomb, sizeof(Coulomb_parameters), MPI_BYTE, 0, comm_cart);

  Coulomb::bcast_coulomb_params();
#endif

#ifdef DIPOLES
  MPI_Bcast(&dipole, sizeof(Dipole_parameters), MPI_BYTE, 0, comm_cart);

  Dipole::set_method_local(dipole.method);

  Dipole::bcast_params(comm_cart);
#endif

  on_coulomb_change();
#endif
}

/****************** REQ_RESCALE_PART ************/

void mpi_rescale_particles(int dir, double scale) {
  int pnode;

  mpi_call(mpi_rescale_particles_slave, -1, dir);
  for (pnode = 0; pnode < n_nodes; pnode++) {
    if (pnode == this_node) {
      local_rescale_particles(dir, scale);
    } else {
      MPI_Send(&scale, 1, MPI_DOUBLE, pnode, SOME_TAG, comm_cart);
    }
  }
  on_particle_change();
}

void mpi_rescale_particles_slave(int, int dir) {
  double scale = 0.0;
  MPI_Recv(&scale, 1, MPI_DOUBLE, 0, SOME_TAG, comm_cart, MPI_STATUS_IGNORE);
  local_rescale_particles(dir, scale);
  on_particle_change();
}

/*************** REQ_BCAST_CS *****************/

void mpi_bcast_cell_structure(int cs) {
  mpi_call(mpi_bcast_cell_structure_slave, -1, cs);
  cells_re_init(cs);
}

void mpi_bcast_cell_structure_slave(int, int cs) { cells_re_init(cs); }

/*************** REQ_BCAST_NPTISO_GEOM *****************/

void mpi_bcast_nptiso_geom() {
  mpi_call(mpi_bcast_nptiso_geom_slave, -1, 0);
  mpi_bcast_nptiso_geom_slave(-1, 0);
}

void mpi_bcast_nptiso_geom_slave(int, int) {
  MPI_Bcast(&nptiso.geometry, 1, MPI_INT, 0, comm_cart);
  MPI_Bcast(&nptiso.dimension, 1, MPI_INT, 0, comm_cart);
  MPI_Bcast(&nptiso.cubic_box, 1, MPI_INT, 0, comm_cart);
  MPI_Bcast(&nptiso.non_const_dim, 1, MPI_INT, 0, comm_cart);
}

/******************* REQ_BCAST_LBPAR ********************/

void mpi_bcast_lb_particle_coupling() {
  mpi_call(mpi_bcast_lb_particle_coupling_slave, 0, 0);
  boost::mpi::broadcast(comm_cart, lb_particle_coupling, 0);
}

/******************* REQ_BCAST_CUDA_GLOBAL_PART_VARS ********************/

void mpi_bcast_cuda_global_part_vars() {
#ifdef CUDA
  mpi_call(mpi_bcast_cuda_global_part_vars_slave, 1,
           0); // third parameter is meaningless
  mpi_bcast_cuda_global_part_vars_slave(-1, 0);
#endif
}

void mpi_bcast_cuda_global_part_vars_slave(int, int) {
#ifdef CUDA
  MPI_Bcast(gpu_get_global_particle_vars_pointer_host(),
            sizeof(CUDA_global_part_vars), MPI_BYTE, 0, comm_cart);
  espressoSystemInterface.requestParticleStructGpu();
#endif
}

/********************* REQ_SET_EXCL ********/
#ifdef EXCLUSIONS
void mpi_send_exclusion_slave(int part1, int part2, int _delete) {
  local_change_exclusion(part1, part2, _delete);
  on_particle_change();
}

REGISTER_CALLBACK(mpi_send_exclusion_slave)

void mpi_send_exclusion(int part1, int part2, int _delete) {
  mpi_call(mpi_send_exclusion_slave, part1, part2, _delete);
  mpi_send_exclusion_slave(part1, part2, _delete);
}
#endif

/********************* REQ_ICCP3M_INIT********/
#ifdef ELECTROSTATICS
void mpi_iccp3m_init_slave(const iccp3m_struct &iccp3m_cfg_) {
#ifdef ELECTROSTATICS
  iccp3m_cfg = iccp3m_cfg_;

  check_runtime_errors();
#endif
}

REGISTER_CALLBACK(mpi_iccp3m_init_slave)

int mpi_iccp3m_init() {
#ifdef ELECTROSTATICS
  mpi_call(mpi_iccp3m_init_slave, iccp3m_cfg);

  return check_runtime_errors();
#else
  return 0;
#endif
}
#endif

Utils::Vector3d mpi_recv_lb_interpolated_velocity(int node,
                                                  Utils::Vector3d const &pos) {
#ifdef LB
  if (this_node == 0) {
    comm_cart.send(node, SOME_TAG, pos);
    mpi_call(mpi_recv_lb_interpolated_velocity_slave, node, 0);
    Utils::Vector3d interpolated_u{};
    comm_cart.recv(node, SOME_TAG, interpolated_u);
    return interpolated_u;
  }
#endif
  return {};
}

void mpi_recv_lb_interpolated_velocity_slave(int node, int) {
#ifdef LB
  if (node == this_node) {
    Utils::Vector3d pos{};
    comm_cart.recv(0, SOME_TAG, pos);
    auto const interpolated_u =
        lb_lbinterpolation_get_interpolated_velocity(pos);
    comm_cart.send(0, SOME_TAG, interpolated_u);
  }
#endif
}

/****************************************************/

void mpi_bcast_max_mu() {
#if defined(DIPOLES) and defined(DP3M)
  mpi_call(mpi_bcast_max_mu_slave, -1, 0);

  calc_mu_max();

#endif
}

void mpi_bcast_max_mu_slave(int, int) {
#if defined(DIPOLES) and defined(DP3M)

  calc_mu_max();

#endif
}

/***** GALILEI TRANSFORM AND ASSOCIATED FUNCTIONS ****/

void mpi_kill_particle_motion(int rotation) {
  mpi_call(mpi_kill_particle_motion_slave, -1, rotation);
  local_kill_particle_motion(rotation);
  on_particle_change();
}

void mpi_kill_particle_motion_slave(int, int rotation) {
  local_kill_particle_motion(rotation);
  on_particle_change();
}

void mpi_kill_particle_forces(int torque) {
  mpi_call(mpi_kill_particle_forces_slave, -1, torque);
  local_kill_particle_forces(torque);
  on_particle_change();
}

void mpi_kill_particle_forces_slave(int, int torque) {
  local_kill_particle_forces(torque);
  on_particle_change();
}

void mpi_system_CMS() {
  int pnode;
  double data[4];
  double rdata[4];
  double *pdata = rdata;

  data[0] = 0.0;
  data[1] = 0.0;
  data[2] = 0.0;
  data[3] = 0.0;

  mpi_call(mpi_system_CMS_slave, -1, 0);

  for (pnode = 0; pnode < n_nodes; pnode++) {
    if (pnode == this_node) {
      local_system_CMS(pdata);
      data[0] += rdata[0];
      data[1] += rdata[1];
      data[2] += rdata[2];
      data[3] += rdata[3];
    } else {
      MPI_Recv(rdata, 4, MPI_DOUBLE, MPI_ANY_SOURCE, SOME_TAG, comm_cart,
               MPI_STATUS_IGNORE);
      data[0] += rdata[0];
      data[1] += rdata[1];
      data[2] += rdata[2];
      data[3] += rdata[3];
    }
  }

  gal.cms[0] = data[0] / data[3];
  gal.cms[1] = data[1] / data[3];
  gal.cms[2] = data[2] / data[3];
}

void mpi_system_CMS_slave(int, int) {
  double rdata[4];
  double *pdata = rdata;
  local_system_CMS(pdata);
  MPI_Send(rdata, 4, MPI_DOUBLE, 0, SOME_TAG, comm_cart);
}

void mpi_system_CMS_velocity() {
  int pnode;
  double data[4];
  double rdata[4];
  double *pdata = rdata;

  data[0] = 0.0;
  data[1] = 0.0;
  data[2] = 0.0;
  data[3] = 0.0;

  mpi_call(mpi_system_CMS_velocity_slave, -1, 0);

  for (pnode = 0; pnode < n_nodes; pnode++) {
    if (pnode == this_node) {
      local_system_CMS_velocity(pdata);
      data[0] += rdata[0];
      data[1] += rdata[1];
      data[2] += rdata[2];
      data[3] += rdata[3];
    } else {
      MPI_Recv(rdata, 4, MPI_DOUBLE, MPI_ANY_SOURCE, SOME_TAG, comm_cart,
               MPI_STATUS_IGNORE);
      data[0] += rdata[0];
      data[1] += rdata[1];
      data[2] += rdata[2];
      data[3] += rdata[3];
    }
  }

  gal.cms_vel[0] = data[0] / data[3];
  gal.cms_vel[1] = data[1] / data[3];
  gal.cms_vel[2] = data[2] / data[3];
}

void mpi_system_CMS_velocity_slave(int, int) {
  double rdata[4];
  double *pdata = rdata;
  local_system_CMS_velocity(pdata);
  MPI_Send(rdata, 4, MPI_DOUBLE, 0, SOME_TAG, comm_cart);
}

void mpi_galilei_transform() {
  double cmsvel[3];

  mpi_system_CMS_velocity();
  memmove(cmsvel, gal.cms_vel, 3 * sizeof(double));

  mpi_call(mpi_galilei_transform_slave, -1, 0);
  MPI_Bcast(cmsvel, 3, MPI_DOUBLE, 0, comm_cart);

  local_galilei_transform(cmsvel);

  on_particle_change();
}

void mpi_galilei_transform_slave(int, int) {
  double cmsvel[3];
  MPI_Bcast(cmsvel, 3, MPI_DOUBLE, 0, comm_cart);

  local_galilei_transform(cmsvel);
  on_particle_change();
}

/******************** REQ_SWIMMER_REACTIONS ********************/

void mpi_setup_reaction() {
#ifdef SWIMMER_REACTIONS
  mpi_call(mpi_setup_reaction_slave, -1, 0);
  local_setup_reaction();
#endif
}

void mpi_setup_reaction_slave(int, int) {
#ifdef SWIMMER_REACTIONS
  local_setup_reaction();
#endif
}

/*********************** MAIN LOOP for slaves ****************/

void mpi_loop() {
  if (this_node != 0)
    mpiCallbacks().loop();
}

/*********************** other stuff ****************/

#ifdef CUDA
std::vector<EspressoGpuDevice> mpi_gather_cuda_devices() {
  mpi_call(mpi_gather_cuda_devices_slave, 0, 0);
  return cuda_gather_gpus();
}
#endif

void mpi_gather_cuda_devices_slave(int, int) {
#ifdef CUDA
  cuda_gather_gpus();
#endif
}

std::vector<int> mpi_resort_particles(int global_flag) {
  mpi_call(mpi_resort_particles_slave, global_flag, 0);
  cells_resort_particles(global_flag);

  std::vector<int> n_parts;
  boost::mpi::gather(comm_cart, cells_get_n_particles(), n_parts, 0);

  return n_parts;
}

void mpi_resort_particles_slave(int global_flag, int) {
  cells_resort_particles(global_flag);

  boost::mpi::gather(comm_cart, cells_get_n_particles(), 0);
}
