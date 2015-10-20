#ifndef CENTRAL2D_H
#define CENTRAL2D_H

#include <cstdio>
#include <cmath>
#include <cassert>
#include <vector>

//ldoc on
/**
 * # Jiang-Tadmor central difference scheme
 * 
 * [Jiang and Tadmor][jt] proposed a high-resolution finite difference
 * scheme for solving hyperbolic PDE systems in two space dimensions.
 * The method is particularly attractive because, unlike many other
 * methods in this space, it does not require that we write any
 * solvers for problems with special initial data (so-called Riemann
 * problems), nor even that we compute Jacobians of the flux
 * functions.
 * 
 * While this code is based loosely on the Fortran code at the end of
 * Jiang and Tadmor's paper, we've written the current code to be
 * physics-agnostic (rather than hardwiring it to the shallow water
 * equations -- or the Euler equations in the Jiang-Tadmor paper).
 * If you're interested in the Euler equations, feel free to add your
 * own physics class to support them!
 * 
 * [jt]: http://www.cscamm.umd.edu/tadmor/pub/central-schemes/Jiang-Tadmor.SISSC-98.pdf
 * 
 * ## Staggered grids
 * 
 * The Jiang-Tadmor scheme works by alternating between a main grid
 * and a staggered grid offset by half a step in each direction.
 * Understanding this is important, particularly if you want to apply
 * a domain decomposition method and batch time steps between
 * synchronization barriers in your parallel code!
 * 
 * In even-numbered steps, the entry `u(i,j)` in the array of solution
 * values represents the average value of a cell centered at a point
 * $(x_i,y_j)$.  At the following odd-numbered step, the same entry
 * represents values for a cell centered at $(x_i + \Delta x/2, y_j +
 * \Delta y/2)$.  However, whenever we run a simulation, we always take
 * an even number of steps, so that outside the solver we can just think
 * about values on the main grid.  If `uold` and `unew` represent the
 * information at two successive *even* time steps (i.e. they represent
 * data on the same grid), then `unew(i,j)` depends indirectly on
 * `u(p,q)` for $i-3 \leq p \leq i+3$ and $j-3 \leq q \leq j+3$.
 * 
 * We currently manage this implicitly: the arrays at even time steps
 * represent cell values on the main grid, and arrays at odd steps
 * represent cell values on the staggered grid.  Our main `run` 
 * function always takes an even number of time steps to ensure we end
 * up on the primary grid.
 * 
 * ## Interface
 * 
 * We want a clean separation between the physics, the solver,
 * and the auxiliary limiter methods used by the solver.  At the same
 * time, we don't want to pay the overhead (mostly in terms of lost
 * optimization opportunities) for calling across an abstraction
 * barrier in the inner loops of our solver.  We can get around this
 * in C++ by providing the solver with *template arguments*, resolved
 * at compile time, that describe separate classes to implement the
 * physics and the limiter.
 *
 * The `Central2D` solver class takes two template arguments:
 * `Physics` and `Limiter`.  For `Physics`, we expect the name of a class
 * that defines:
 * 
 *  - A type for numerical data (`real`)
 *  - A type for solution and flux vectors in each cell (`vec`)
 *  - A flux computation function (`flux(vec& F, vec& G, const vec& U)`)
 *  - A wave speed computation function 
 *    (`wave_speed(real& cx, real& cy, const vec& U)`).
 * 
 * The `Limiter` argument is a type with a static function `limdiff`
 * with the signature
 * 
 *         limdiff(fm, f0, fp)
 * 
 * The semantics are that `fm`, `f0`, and `fp` are three successive
 * grid points in some direction, and the function returns an approximate
 * (scaled) derivative value from these points.
 * 
 * The solver keeps arrays for the solution, flux values, derivatives
 * of the solution and the fluxes, and the solution at the next time
 * point.  We use the C++ `vector` class to manage storage for these
 * arrays; but since we want to think of them as 2D arrays, we also
 * provide convenience functions to access them with multiple indices
 * (though we maintain C-style 0-based indexing).  The internal arrays
 * are padded with ghost cells; the ghost cell in the lower left corner
 * of the domain has index (0,0).
 */

template <class Physics, class Limiter>
class Central2D {
public:
    typedef typename Physics::real real;
    typedef typename Physics::vec  vec;

 Central2D(real w, real h,     // Domain width / height
	   int nx, int ny,     // Number of cells in x/y (without ghosts)
	   int nx_subdg, int ny_subdg,
	   int nx_subd, int ny_subd,
	   int imin, int imax, int jmin, int jmax,
	   int NT,
	   real cfl = 0.45) :  // Max allowed CFL number
    
    nx(nx), ny(ny), NT(NT),
      nx_subdg(nx_subdg),ny_subdg(ny_subdg),
      nx_subd(nx_subd),ny_subd(ny_subd),
      nx_all(nx + 2*nghost),
      ny_all(ny + 2*nghost),
      dx(w/nx), dy(h/ny),
      cfl(cfl), 
      u_ (nx_all * ny_all),
      u1_(nx_all * ny_all),
      ud_(nx_subdg * ny_subdg),
      f_ (nx_subdg * ny_subdg),
      g_ (nx_subdg * ny_subdg),
      ux_(nx_subdg * ny_subdg),
      uy_(nx_subdg * ny_subdg),
      fx_(nx_subdg * ny_subdg),
      gy_(nx_subdg * ny_subdg),
      v_ (nx_subdg * ny_subdg)
	{}

    // Advance from time 0 to time tfinal
    void run(real tfinal);

    // Call f(Uxy, x, y) at each cell center to set initial conditions
    template <typename F>
    void init(F f);

    // Diagnostics
    void solution_check();

    // Array size accessors
    int xsize() const { return nx; }
    int ysize() const { return ny; }
    
    // Read / write elements of simulation state
    vec&       operator()(int i, int j) {
        return u_[offset(i+nghost,j+nghost)];
    }
    
    const vec& operator()(int i, int j) const {
        return u_[offset(i+nghost,j+nghost)];
    }
    
private:
    static constexpr int nghost = 3;   // Number of ghost cells

    const int nx, ny;          // Number of (non-ghost) cells in x/y
    const int nx_subdg,ny_subdg;
    const int NT;
    const int imin,imax,jmin,jmax;
    const int nx_all, ny_all;  // Total cells in x/y (including ghost)
    const real dx, dy;         // Cell size in x/y
    const real cfl;            // Allowed CFL number

    std::vector<vec> u_;            // Solution values
    std::vector<vec> u1_;            // Solution values
    std::vector<vec> ud_;            // Solution values
    std::vector<vec> f_;            // Fluxes in x
    std::vector<vec> g_;            // Fluxes in y
    std::vector<vec> ux_;           // x differences of u
    std::vector<vec> uy_;           // y differences of u
    std::vector<vec> fx_;           // x differences of f
    std::vector<vec> gy_;           // y differences of g
    std::vector<vec> v_;            // Solution values at next step

    // Array accessor functions

    int offset(int ix, int iy) const { return iy*nx_subdg+ix; }

    vec& u(int ix, int iy)    { return u_[offset(ix,iy)]; }
    vec& u1(int ix, int iy)    { return u_[offset(ix,iy)]; }
    vec& ud(int ix, int iy)    { return u_[offset(ix,iy)]; }
    vec& v(int ix, int iy)    { return v_[offset(ix,iy)]; }
    vec& f(int ix, int iy)    { return f_[offset(ix,iy)]; }
    vec& g(int ix, int iy)    { return g_[offset(ix,iy)]; }

    vec& ux(int ix, int iy)   { return ux_[offset(ix,iy)]; }
    vec& uy(int ix, int iy)   { return uy_[offset(ix,iy)]; }
    vec& fx(int ix, int iy)   { return fx_[offset(ix,iy)]; }
    vec& gy(int ix, int iy)   { return gy_[offset(ix,iy)]; }

    // Wrapped accessor (periodic BC)
    int ioffset(int ix, int iy) {
        return offset( (ix+nx-nghost) % nx + nghost,
                       (iy+ny-nghost) % ny + nghost );
    }

    vec& uwrap(int ix, int iy)  { return u1_[ioffset(ix,iy)]; }

    // Apply limiter to all components in a vector
    static void limdiff(vec& du, const vec& um, const vec& u0, const vec& up) {
        for (int m = 0; m < du.size(); ++m)
            du[m] = Limiter::limdiff(um[m], u0[m], up[m]);
    }

    // Stages of the main algorithm
    void apply_periodic(int& imin, int& imax, int& jmin, int& jmax);
    void compute_fg_speeds(real& cx, real& cy);
    void limited_derivs();
    void compute_step(int io, real dt);

};


/**
 * ## Initialization
 * 
 * Before starting the simulation, we need to be able to set the
 * initial conditions.  The `init` function does exactly this by
 * running a callback function at the center of each cell in order
 * to initialize the cell $U$ value.  For the purposes of this function,
 * cell $(i,j)$ is the subdomain 
 * $[i \Delta x, (i+1) \Delta x] \times [j \Delta y, (j+1) \Delta y]$.
 */

template <class Physics, class Limiter>
template <typename F>
void Central2D<Physics, Limiter>::init(F f)
{
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix)
            f(u(nghost+ix,nghost+iy), (ix+0.5)*dx, (iy+0.5)*dy);
}

/**
 * ## Time stepper implementation
 * 
 * ### Boundary conditions
 * 
 * In finite volume methods, boundary conditions are typically applied by
 * setting appropriate values in ghost cells.  For our framework, we will
 * apply periodic boundary conditions; that is, waves that exit one side
 * of the domain will enter from the other side.
 * 
 * We apply the conditions by assuming that the cells with coordinates
 * `nghost <= ix <= nx+nghost` and `nghost <= iy <= ny+nghost` are
 * "canonical", and setting the values for all other cells `(ix,iy)`
 * to the corresponding canonical values `(ix+p*nx,iy+q*ny)` for some
 * integers `p` and `q`.
 */

template <class Physics, class Limiter>
  void Central2D<Physics, Limiter>::apply_periodic(imin,imax,jmin,jmax)
{
  // Copy data between right and left boundaries

  if(imax == nx+nghost-1) {
    for (int iy = 0; iy < ny_all; ++iy)
      for (int ix = 0; ix < nghost; ++ix) {
	u1(nx+nghost+ix,iy) = uwrap(nx+nghost+ix,iy);
      }
  }

  if(imin == nghost) {
    for (int iy = 0; iy < ny_all; ++iy)
      for (int ix = 0; ix < nghost; ++ix) {
	u1(ix,          iy) = uwrap(ix,          iy);
      }
  }

  if(jmax == ny+nghost-1) {
    for (int iy = 0; iy < ny_all; ++iy)
      for (int ix = 0; ix < nghost; ++ix) {
	u1(ix,ny+nghost+iy) = uwrap(ix,ny+nghost+iy);
      }
  }

  if(jmin == nghost) {
    for (int iy = 0; iy < ny_all; ++iy)
      for (int ix = 0; ix < nghost; ++ix) {
	u1(ix,          iy) = uwrap(ix,          iy);
      }
  }
}

/**
 * ### Initial flux and speed computations
 * 
 * At the start of each time step, we need the flux values at
 * cell centers (to advance the numerical method) and a bound
 * on the wave speeds in the $x$ and $y$ directions (so that
 * we can choose a time step that respects the specified upper
 * bound on the CFL number).
 */

template <class Physics, class Limiter>
void Central2D<Physics, Limiter>::compute_fg_speeds(real& cx_, real& cy_)
{
    using namespace std;
    real cx = 1.0e-15;
    real cy = 1.0e-15;
    for (int iy = 0; iy < ny_subdg; ++iy)
        for (int ix = 0; ix < nx_subdg; ++ix) {
            real cell_cx, cell_cy;
            Physics::flux(f(ix,iy), g(ix,iy), ud(ix,iy));
            Physics::wave_speed(cell_cx, cell_cy, ud(ix,iy));
            cx = max(cx, cell_cx);
            cy = max(cy, cell_cy);
        }
    cx_ = cx;
    cy_ = cy;
}

/**
 * ### Derivatives with limiters
 * 
 * In order to advance the time step, we also need to estimate
 * derivatives of the fluxes and the solution values at each cell.
 * In order to maintain stability, we apply a limiter here.
 */

template <class Physics, class Limiter>
void Central2D<Physics, Limiter>::limited_derivs()
{
    for (int iy = 1; iy < ny_subdg-1; ++iy)
        for (int ix = 1; ix < nx_subdg-1; ++ix) {

            // x derivs
            limdiff( ux(ix,iy), ud(ix-1,iy), ud(ix,iy), ud(ix+1,iy) );
            limdiff( fx(ix,iy), f(ix-1,iy), f(ix,iy), f(ix+1,iy) );

            // y derivs
            limdiff( uy(ix,iy), ud(ix,iy-1), ud(ix,iy), ud(ix,iy+1) );
            limdiff( gy(ix,iy), g(ix,iy-1), g(ix,iy), g(ix,iy+1) );
        }
}


/**
 * ### Advancing a time step
 * 
 * Take one step of the numerical scheme.  This consists of two pieces:
 * a first-order corrector computed at a half time step, which is used
 * to obtain new $F$ and $G$ values; and a corrector step that computes
 * the solution at the full step.  For full details, we refer to the
 * [Jiang and Tadmor paper][jt].
 * 
 * The `compute_step` function takes two arguments: the `io` flag
 * which is the time step modulo 2 (0 if even, 1 if odd); and the `dt`
 * flag, which actually determines the time step length.  We need
 * to know the even-vs-odd distinction because the Jiang-Tadmor
 * scheme alternates between a primary grid (on even steps) and a
 * staggered grid (on odd steps).  This means that the data at $(i,j)$
 * in an even step and the data at $(i,j)$ in an odd step represent
 * values at different locations in space, offset by half a space step
 * in each direction.  Every other step, we shift things back by one
 * mesh cell in each direction, essentially resetting to the primary
 * indexing scheme.
 */

template <class Physics, class Limiter>
void Central2D<Physics, Limiter>::compute_step(int io, real dt)
{
    real dtcdx2 = 0.5 * dt / dx;
    real dtcdy2 = 0.5 * dt / dy;

    // Predictor (flux values of f and g at half step)
    for (int iy = 1; iy < ny_subdg-1; ++iy)
        for (int ix = 1; ix < nx_subdg-1; ++ix) {
            vec uh = u(ix,iy);
            for (int m = 0; m < uh.size(); ++m) {
                uh[m] -= dtcdx2 * fx(ix,iy)[m];
                uh[m] -= dtcdy2 * gy(ix,iy)[m];
            }
            Physics::flux(f(ix,iy), g(ix,iy), uh);
        }

    // Corrector (finish the step)
    for (int iy = nghost-io; iy < ny_subd+nghost-io; ++iy)
        for (int ix = nghost-io; ix < nx_subd+nghost-io; ++ix) {
            for (int m = 0; m < v(ix,iy).size(); ++m) {
                v(ix,iy)[m] =
                    0.2500 * ( ud(ix,  iy)[m] + ud(ix+1,iy  )[m] +
                               ud(ix,iy+1)[m] + ud(ix+1,iy+1)[m] ) -
                    0.0625 * ( ux(ix+1,iy  )[m] - ux(ix,iy  )[m] +
                               ux(ix+1,iy+1)[m] - ux(ix,iy+1)[m] +
                               uy(ix,  iy+1)[m] - uy(ix,  iy)[m] +
                               uy(ix+1,iy+1)[m] - uy(ix+1,iy)[m] ) -
                    dtcdx2 * ( f(ix+1,iy  )[m] - f(ix,iy  )[m] +
                               f(ix+1,iy+1)[m] - f(ix,iy+1)[m] ) -
                    dtcdy2 * ( g(ix,  iy+1)[m] - g(ix,  iy)[m] +
                               g(ix+1,iy+1)[m] - g(ix+1,iy)[m] );
            }
        }

    // Copy from v storage back to main grid
    for (int j = nghost; j < ny_subd+nghost; ++j){
        for (int i = nghost; i < nx_subd+nghost; ++i){
            u(i,j) = v(i-io,j-io);
        }
    }
}


/**
 * ### Advance time
 * 
 * The `run` method advances from time 0 (initial conditions) to time
 * `tfinal`.  Note that `run` can be called repeatedly; for example,
 * we might want to advance for a period of time, write out a picture,
 * advance more, and write another picture.  In this sense, `tfinal`
 * should be interpreted as an offset from the time represented by
 * the simulator at the start of the call, rather than as an absolute time.
 * 
 * We always take an even number of steps so that the solution
 * at the end lives on the main grid instead of the staggered grid. 
 */

template <class Physics, class Limiter>
  void Central2D<Physics, Limiter>::run(real tfinal)
{

  // Timings
  real t = 0;
  real dt;

#pragma omp parallel private(ix,iy,m,io,imin,imax,jmin,jmax,iming,imaxg,jming,jmaxg,irank,cx,cy,done,ud,f,g,ux,uy,fx,gy)
  {
    bool done = false;

    // Set up thread specific variables
    int irank = omp_get_thread_num();

    imin = nghost + (mod(irank % npx) - 1)*nx_subd;
    imax = imin + nx_subd - 1;
    jmin = nghost + floor((irank - 1)/npx)*yvals;
    jmax = jmin + ny_subd - 1;

    // These nghost here are based on wave speed, can change
    int iming = imin - nghost;
    int imaxg = imax + nghost;
    int jming = jmin - nghost;
    int jmaxg = jmax + nghost;

    while (!done) {

      for (int io = 0; io < 2; ++io) {
	real cx, cy;

	// Read from base into local copy
	ud(0:nx_subdg-1,0:ny_subdg-1) = u(iming:imaxg,jming:jmaxg);

	compute_fg_speeds(cx, cy); //////////
	limited_derivs(); //////////
	
	if (io == 0) {
	  dt_array(irank) = cfl / std::max(cx/dx, cy/dy);
	}
      }

      // Get timing array
      #pragma omp barrier

      // Update timestep for this computation
      dt = std::max_element(dt_array.begin(),dt_array.end());
      if (t + 2*dt >= tfinal) {
	dt = (tfinal-t)/2;
	done = true;
      }

      // Compute
      compute_step(io, dt); //////////

      //Copy back subdomain to placeholder
      u1(imin:imax,jmin:jmax) = ud(nghost:nxsubd-1,nghost:nysubd-1);
      // Apply periodic BCs for necessary regions
      apply_periodic(imin,imax,jmin,jmax);

      // Wait until everything completes solving and copying
      #pragma omp barrier

      // End of iteration updates - copy back
      #pragma omp single
      {
	t += dt;
	u = u1;
      }
    }
  }
}

/**
 * ### Diagnostics
 * 
 * The numerical method is supposed to preserve (up to rounding
 * errors) the total volume of water in the domain and the total
 * momentum.  Ideally, we should also not see negative water heights,
 * since that will cause the system of equations to blow up.  For
 * debugging convenience, we'll plan to periodically print diagnostic
 * information about these conserved quantities (and about the range
 * of water heights).
 */

template <class Physics, class Limiter>
void Central2D<Physics, Limiter>::solution_check()
{
    using namespace std;
    real h_sum = 0, hu_sum = 0, hv_sum = 0;
    real hmin = u(nghost,nghost)[0];
    real hmax = hmin;
    for (int j = nghost; j < ny+nghost; ++j)
        for (int i = nghost; i < nx+nghost; ++i) {
            vec& uij = u(i,j);
            real h = uij[0];
            h_sum += h;
            hu_sum += uij[1];
            hv_sum += uij[2];
            hmax = max(h, hmax);
            hmin = min(h, hmin);
            assert( h > 0) ;
        }
    real cell_area = dx*dy;
    h_sum *= cell_area;
    hu_sum *= cell_area;
    hv_sum *= cell_area;
    printf("-\n  Volume: %g\n  Momentum: (%g, %g)\n  Range: [%g, %g]\n",
           h_sum, hu_sum, hv_sum, hmin, hmax);
}

//ldoc off
#endif /* CENTRAL2D_H*/
