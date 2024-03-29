/*
** Code to implement a d2q9-bgk lattice boltzmann scheme.
** 'd2' inidates a 2-dimensional grid, and
** 'q9' indicates 9 velocities per grid cell.
** 'bgk' refers to the Bhatnagar-Gross-Krook collision step.
**
** The 'speeds' in each cell are numbered as follows:
**
** 6 2 5
**  \|/
** 3-0-1
**  /|\
** 7 4 8
**
** A 2D grid:
**
**           cols
**       --- --- ---
**      | D | E | F |
** rows  --- --- ---
**      | A | B | C |
**       --- --- ---
**
** 'unwrapped' in row major order to give a 1D array:
**
**  --- --- --- --- --- ---
** | A | B | C | D | E | F |
**  --- --- --- --- --- ---
**
** Grid indicies are:
**
**          ny
**          ^       cols(ii)
**          |  ----- ----- -----
**          | | ... | ... | etc |
**          |  ----- ----- -----
** rows(jj) | | 1,0 | 1,1 | 1,2 |
**          |  ----- ----- -----
**          | | 0,0 | 0,1 | 0,2 |
**          |  ----- ----- -----
**          ----------------------> nx
**
** Note the names of the input parameter and obstacle files
** are passed on the command line, e.g.:
**
**   ./d2q9-bgk input.params obstacles.dat
**
** Be sure to adjust the grid dimensions in the parameter file
** if you choose a different obstacle file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mpi.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

#define NSPEEDS         9
#define FINALSTATEFILE  "final_state.dat"
#define AVVELSFILE      "av_vels.dat"

/* struct to hold the parameter values */
typedef struct
{
  int    nx;            /* no. of cells in x-direction */
  int    ny;            /* no. of cells in y-direction */
  int    maxIters;      /* no. of iterations */
  int    reynolds_dim;  /* dimension for Reynolds number */
  float density;       /* density per link */
  float accel;         /* density redistribution */
  float omega;         /* relaxation parameter */
} t_param;

/* struct to hold the 'speed' values */
typedef struct
{
  float speeds[NSPEEDS];
} t_speed;

/*
** function prototypes
*/

/* load params, allocate memory, load obstacles & initialise fluid particle densities */
int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr);

/*
** The main calculation methods.
** timestep calls, in order, the functions:
** accelerate_flow(), propagate(), rebound() & collision()
*/
int timestep(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);
int accelerate_flow(const t_param params, t_speed* cells, int* obstacles);
int propagate(const t_param params, t_speed* cells, t_speed* tmp_cells);
int rebound(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);
int collision(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);
int write_values(const t_param params, t_speed* cells, int* obstacles, double* av_vels);
int calc_ncols_from_rank(int rank, int size, int nx);
void initialise_params_from_file(const char* paramfile, t_param* params);
void test_run(const char* output_file, int nx, int ny, t_speed *cells, int *obstacles);
int test_files(const char* file1, const char* file2, int nx, int ny, t_speed *cells, int *obstacles);
void test_vels(const char* output_file, double *vels, int steps);
void output_state(const char* output_file, int step, t_speed *cells, int *obstacles, int nx, int ny);

/* finalise, including freeing up allocated memory */
int finalise(const t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
             int** obstacles_ptr, double** av_vels_ptr);

/* Sum all the densities in the grid.
** The total should remain constant from one timestep to the next. */
float total_density(const t_param params, t_speed* cells);

/* compute average velocity */
double av_velocity(const t_param params, t_speed* cells, int* obstacles);

/* calculate Reynolds number */
float calc_reynolds(const t_param params, t_speed* cells, int* obstacles);

/* utility functions */
void die(const char* message, const int line, const char* file);
void usage(const char* exe);

/*
** main program:
** initialise, timestep loop, finalise
*/
int main(int argc, char* argv[])
{
  char*    paramfile = NULL;    /* name of the input parameter file */
  char*    obstaclefile = NULL; /* name of a the input obstacle file */
  t_param  params;              /* struct to hold parameter values */
  t_speed* cells     = NULL;    /* grid containing fluid densities */
  t_speed* tmp_cells = NULL;    /* scratch space */
  int*     obstacles = NULL;    /* grid indicating which cells are blocked */
  double* av_vels   = NULL;     /* a record of the av. velocity computed for each timestep */
  struct timeval timstr;        /* structure to hold elapsed time */
  struct rusage ru;             /* structure to hold CPU time--system and user */
  double tic, toc;              /* floating point numbers to calculate elapsed wallclock time */
  double usrtim;                /* floating point number to record elapsed user CPU time */
  double systim;                /* floating point number to record elapsed system CPU time */
  unsigned long long int flow_cells = 0; // number of cells without obstacles

  //MPI related
  int rank;               /* 'rank' of process among it's cohort */
  int size;               /* size of cohort, i.e. num processes started */
  int flag;               /* for checking whether MPI_Init() has been called */
  int strlen;             /* length of a character array */
  enum bool {FALSE,TRUE}; /* enumerated type: false = 0, true = 1 */
  char hostname[MPI_MAX_PROCESSOR_NAME];  /* character array to hold hostname running process */

  /* initialise our MPI environment */
  MPI_Init( &argc, &argv );

  /* check whether the initialisation was successful */
  MPI_Initialized(&flag);
  if ( flag != TRUE ) {
    MPI_Abort(MPI_COMM_WORLD,EXIT_FAILURE);
  }

  /* determine the hostname */
  MPI_Get_processor_name(hostname,&strlen);

  /*
  ** determine the SIZE of the group of processes associated with
  ** the 'communicator'.  MPI_COMM_WORLD is the default communicator
  ** consisting of all the processes in the launched MPI 'job'
  */
  MPI_Comm_size( MPI_COMM_WORLD, &size );
  printf("SIZE: %d\n", size);

  /* determine the RANK of the current process [0:SIZE-1] */
  MPI_Comm_rank( MPI_COMM_WORLD, &rank );

  /* parse the command line */
  if (argc != 3)
  {
    usage(argv[0]);
  }
  else
  {
    paramfile = argv[1];
    obstaclefile = argv[2];
  }



  /* iterate for maxIters timesteps */
  gettimeofday(&timstr, NULL);
  tic = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  //MPI process subgrid
  initialise_params_from_file(paramfile, &params);
  int process_rows = calc_ncols_from_rank(rank, size, params.ny);
  t_param  process_params = params;  // copy values
  process_params.ny = process_rows + 2; //add 2 for halo exchanges

  /* main grid */
  int sss = sizeof(t_speed) * (params.ny * params.nx);
  printf("NUMER OF PROCESSES: %d\n", size);
  printf("RANK: %d\n", rank);
  printf("BYTES: %d %d %d %d\n\n\n", (int)sizeof(t_speed), process_params.ny, process_params.nx, sss);
  av_vels = (double*)malloc(sizeof(double) * params.maxIters);
  t_speed *process_cells = (t_speed*)calloc((process_params.ny * process_params.nx), sizeof(t_speed));

  if (process_cells == NULL) die("cannot allocate memory for cells", __LINE__, __FILE__);

  /* 'helper' grid, used as scratch space */
  t_speed *process_tmp_cells = (t_speed*)malloc(sizeof(t_speed) * (process_params.ny * process_params.nx));

  if (process_tmp_cells == NULL) die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  int *process_obstacles = calloc((process_params.ny * process_params.nx), sizeof(int));

  if (process_obstacles == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  float* sendbuf_cells = (float*)malloc(sizeof(float) * NSPEEDS * process_params.nx);
  int* sendbuf_obstacles = (int*)malloc(sizeof(int) * process_params.nx);
  float* recvbuf_cells = (float*)malloc(sizeof(float) * NSPEEDS * process_params.nx);
  int* recvbuf_obstacles = (int*)malloc(sizeof(int) * process_params.nx);
  double* sendbuf_av_vels = (double*)malloc(sizeof(double) * process_params.maxIters);
  double* recvbuf_av_vels = (double*)malloc(sizeof(double) * process_params.maxIters);

  if(rank == 0) {
    /* initialise our data structures and load values from file */
    initialise(paramfile, obstaclefile, &params, &cells, &tmp_cells, &obstacles);
    float c = 1;
    for(int i = 0; i < params.ny; ++i) {
      for(int j = 0; j < params.nx; ++j) {
        for(int z = 0; z < 9; ++z) {
          //cells[i*params.nx + j].speeds[z] = c;
        }
        ++c;
        if(!obstacles[i*params.nx + j]) {
          ++flow_cells;
        }
      }
    }
    test_run("TEST_initial_vals.txt", params.nx, params.ny, cells, obstacles);
    //fill in process grid for master process

    for(int i = 1; i < process_params.ny-1; ++i) {
      printf("NX IS %d\n", process_params.nx);
      for(int j = 0; j < process_params.nx; ++j) {
        process_cells[i*(process_params.nx) + j] = cells[(i-1)*params.nx + j]; // account for halo exchange top row with -1
        process_tmp_cells[i*(process_params.nx) + j] = tmp_cells[(i-1)*params.nx + j];
        process_obstacles[i*(process_params.nx) + j] = obstacles[(i-1)*params.nx + j];
      }
    }

    //fill other processes' grids
    for(int i = 1; i < size; ++i) {
      int i_process_rows = calc_ncols_from_rank(i, size, params.ny);
      int rows_per_rank = params.ny / size;
      for(int j = i*rows_per_rank; j < i*rows_per_rank + i_process_rows; ++j) {
        for(int k = 0; k < process_params.nx; ++k) {
          for(int z = 0; z < NSPEEDS; ++z) {
            sendbuf_cells[k*NSPEEDS + z] = cells[j*params.nx + k].speeds[z];
          }
        }
        MPI_Ssend(sendbuf_cells, process_params.nx*NSPEEDS, MPI_FLOAT, i, 0, MPI_COMM_WORLD);
        for(int k = 0; k < process_params.nx; ++k) {
          sendbuf_obstacles[k] = obstacles[j*params.nx + k];
        }
        MPI_Ssend(sendbuf_obstacles, process_params.nx, MPI_INT, i, 1, MPI_COMM_WORLD);
      }
    }
  } else {
    //receive initial values
    for(int j = 1; j < process_params.ny-1; ++j) {
      //printf("rank %d waiting to receive its col %d\n", rank, j);
      MPI_Recv(recvbuf_cells, process_params.nx*NSPEEDS, MPI_FLOAT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      for(int i = 0; i < process_params.nx; ++i) {
        t_speed speeds;
        for(int z = 0; z < NSPEEDS; ++z) {
          speeds.speeds[z] = recvbuf_cells[i*NSPEEDS + z];
        }
        process_cells[j*process_params.nx + i] = speeds;
      }
      MPI_Recv(recvbuf_obstacles, process_params.nx, MPI_INT, 0, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      for(int i = 0; i < process_params.nx; ++i) {
        process_obstacles[j*process_params.nx + i] = recvbuf_obstacles[i];
      }
    }
  }
  if(rank == 0) {
    //DEBUG start
    /*for(int i = 0; i < params.ny; ++i) {
      for(int j = 0; j < params.nx; ++j) {
        //printf("%f %f %d ", cells[i*params.nx + j].speeds[0], cells[i*params.nx + j].speeds[1], obstacles[i*params.nx + j]);
        for(int z = 0; z < NSPEEDS; ++z) {
          cells[i*params.nx + j].speeds[z] = -3;
        }
        obstacles[i*params.nx + j] = -3;
      }*/
      //printf("\n\n");
    //}
    test_run("TEST_intermediate_vals.txt", params.nx, params.ny, cells, obstacles);
  }
  //params.maxIters = 0;
  //DEBUG END


  // start work
  double initial_vel = av_velocity(process_params, process_cells, process_obstacles);
  printf("INITIAL VEOCITY: %.12f\n", (float)initial_vel);
  printf("flow %d\n", (int)flow_cells);
  //params.maxIters = 0;
  char file_name[1024];
  sprintf(file_name, "state_size_%d_proc_%d.txt", size, rank);
  FILE *fp = fopen(file_name, "w");
  fclose(fp);
  for (int tt = 0; tt < params.maxIters; tt++)
  {
    //output_state(file_name, tt, process_cells, process_obstacles, process_params.nx, process_params.ny);
    if(rank == 0 && tt % 500 == 0) printf("iteration: %d\n", tt);
    //exchange halos
    int left = (rank == 0) ? (rank + size - 1) : (rank - 1); // left is bottom, right is top equiv
    int right = (rank + 1) % size;
    //send to the left, receive from right
    //fill with left col
    //printf("LEFT %d, RIGHT %d\n", left, right);
    for(int i = 0; i < process_params.nx; ++i) {
      for(int z = 0; z < NSPEEDS; ++z) {
        sendbuf_cells[i*NSPEEDS + z] = process_cells[1*process_params.nx + i].speeds[z];
      }
      sendbuf_obstacles[i] = process_obstacles[1*process_params.nx + i];
    }

    MPI_Sendrecv(sendbuf_cells,  process_params.nx*NSPEEDS, MPI_FLOAT, left, 0, recvbuf_cells,
                process_params.nx*NSPEEDS, MPI_FLOAT, right, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Sendrecv(sendbuf_obstacles,  process_params.nx, MPI_INT, left, 1, recvbuf_obstacles,
                process_params.nx, MPI_INT, right, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    //populate right col
    for(int i = 0; i < process_params.nx; ++i) {
      t_speed speeds;
      for(int z = 0; z < NSPEEDS; ++z) {
        speeds.speeds[z] = recvbuf_cells[i*NSPEEDS + z];
      }
      process_cells[(process_params.ny-1)*process_params.nx + i] = speeds;
      process_obstacles[(process_params.ny-1)*process_params.nx + i] = recvbuf_obstacles[i];
    }
    //send to right, receive from left
    //fill with right col
    for(int i = 0; i < process_params.nx; ++i) {
      for(int z = 0; z < NSPEEDS; ++z) {
        sendbuf_cells[i*NSPEEDS + z] = process_cells[(process_params.ny-2)*process_params.nx + i].speeds[z];
      }
      sendbuf_obstacles[i] = process_obstacles[(process_params.ny-2)*process_params.nx + i];
    }
    MPI_Sendrecv(sendbuf_cells,  process_params.nx*NSPEEDS, MPI_FLOAT, right, 0, recvbuf_cells,
                process_params.nx*NSPEEDS, MPI_FLOAT, left, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Sendrecv(sendbuf_obstacles,  process_params.nx, MPI_INT, right, 1, recvbuf_obstacles,
                process_params.nx, MPI_INT, left, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    //populate left col
    for(int i = 0; i < process_params.nx; ++i) {
      t_speed speeds;
      for(int z = 0; z < NSPEEDS; ++z) {
        speeds.speeds[z] = recvbuf_cells[i*NSPEEDS + z];
      }
      process_cells[i] = speeds;
      process_obstacles[i] = recvbuf_obstacles[i];
    }

    //output_state(file_name, tt, process_cells, process_obstacles, process_params.nx, process_params.ny);

    //now do computations
    timestep(process_params, process_cells, process_tmp_cells, process_obstacles);
    //initial_vel = av_velocity(process_params, process_cells, process_obstacles);
    //printf("INITIAL VEOCITY: %.12f\n", (float)initial_vel);
    av_vels[tt] = av_velocity(process_params, process_cells, process_obstacles);
#ifdef DEBUG
    printf("==timestep: %d==\n", tt);
    printf("av velocity: %.12E\n", av_vels[tt]);
    printf("tot density: %.12E\n", total_density(params, cells));
#endif
  }

  //receive values in master
  if(rank == 0) {
    for(int i = 1; i < process_params.ny-1; ++i) {
      for(int j = 0; j < process_params.nx; ++j) {
        cells[(i-1)*params.nx + j] = process_cells[i*(process_params.nx) + j]; // account for halo exchange left col with -1
        tmp_cells[(i-1)*params.nx + j] = process_tmp_cells[i*(process_params.nx) + j];
        obstacles[(i-1)*params.nx + j] = process_obstacles[i*(process_params.nx) + j];
      }
    }


    //get other processes' grids
    for(int i = 1; i < size; ++i) {
      int i_process_rows = calc_ncols_from_rank(i, size, params.ny);
      int rows_per_rank = params.ny / size;
      for(int j = i*rows_per_rank; j < i*rows_per_rank + i_process_rows; ++j) {
        MPI_Recv(recvbuf_cells, process_params.nx*NSPEEDS, MPI_FLOAT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for(int k = 0; k < process_params.nx; ++k) {
          t_speed speeds;
          for(int z = 0; z < NSPEEDS; ++z) {
            speeds.speeds[z] = recvbuf_cells[k*NSPEEDS + z];
          }
          cells[j*params.nx + k] = speeds;
        }
        MPI_Recv(recvbuf_obstacles, process_params.nx, MPI_INT, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for(int k = 0; k < process_params.nx; ++k) {
          obstacles[j*params.nx + k] = recvbuf_obstacles[k];
        }
      }
    }

    //get av_vels from processes
    printf("MAX ITERS: %d\n", process_params.maxIters);
    for(int i = 1; i < size; ++i) {
      for(int i = 0; i < process_params.maxIters; ++i) {
        recvbuf_av_vels[i] = 999;
      }
      MPI_Recv(recvbuf_av_vels, process_params.maxIters, MPI_DOUBLE, i, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      for(int j = 0; j < process_params.maxIters; ++j) {
        av_vels[j] += recvbuf_av_vels[j];
      }
    }

    for(int i = 0; i < process_params.maxIters; ++i) {
      av_vels[i] /= (double) flow_cells*100;
    }

    printf("FLOW CELLS: %d\n", (int)flow_cells);
    test_vels("velocities_tot_u.txt", av_vels, process_params.maxIters);
  } else {
    //send final values
    for(int j = 1; j < process_params.ny-1; ++j) {
      for(int i = 0; i < process_params.nx; ++i) {
        for(int z = 0; z < NSPEEDS; ++z) {
          sendbuf_cells[i*NSPEEDS + z] = process_cells[j*process_params.nx + i].speeds[z];
        }
      }
      MPI_Ssend(sendbuf_cells, process_params.nx*NSPEEDS, MPI_FLOAT, 0, 0, MPI_COMM_WORLD);

      for(int i = 0; i < process_params.nx; ++i) {
        //process_obstacles[i*process_params.nx + j + 1] = recvbuf[i];
        sendbuf_obstacles[i] = process_obstacles[j*process_params.nx + i];
      }
      MPI_Ssend(sendbuf_obstacles, process_params.nx, MPI_INT, 0, 1, MPI_COMM_WORLD);
    }
    //send av_vels to master
    for(int i = 0; i < process_params.maxIters; ++i) {
      sendbuf_av_vels[i] = av_vels[i];
    }
    MPI_Ssend(sendbuf_av_vels, process_params.maxIters, MPI_DOUBLE, 0, 2, MPI_COMM_WORLD);
  }

  //DEBUG start
  if(rank == 0) {
    test_run("TEST_final_vals.txt", params.nx, params.ny, cells, obstacles);
    output_state(file_name, 999, cells, obstacles, params.nx, params.ny);
    /*int valid = test_files("TEST_initial_vals.txt", "TEST_final_vals.txt", params.nx, params.ny, cells, obstacles);
    if(valid == 1) {
      printf("VALID\n");
    } else {
      printf("NOT VALID\n");
    }*/
    /*printf("FINAL VALS:\n");
    for(int i = 0; i < 5; ++i) {
      for(int j = 1; j < 5; ++j) {
        printf("%f %f %d ", cells[i*params.nx + j].speeds[0], cells[i*params.nx + j].speeds[1], obstacles[i*params.nx + j]);
      }
      printf("\n\n");
    }*/
  }
  //DEBUG END

  if(rank == 0) {
    gettimeofday(&timstr, NULL);
    toc = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
    getrusage(RUSAGE_SELF, &ru);
    timstr = ru.ru_utime;
    usrtim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
    timstr = ru.ru_stime;
    systim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

    /* write final values and free memory */
    printf("==done==\n");
    printf("Reynolds number:\t\t%.12E\n", calc_reynolds(params, cells, obstacles));
    printf("Elapsed time:\t\t\t%.6lf (s)\n", toc - tic);
    printf("Elapsed user CPU time:\t\t%.6lf (s)\n", usrtim);
    printf("Elapsed system CPU time:\t%.6lf (s)\n", systim);
    write_values(params, cells, obstacles, av_vels);
    finalise(&params, &cells, &tmp_cells, &obstacles, &av_vels);
  }

  free(process_cells);
  free(process_tmp_cells);
  free(process_obstacles);
  /* finialise the MPI enviroment */
  MPI_Finalize();

  return EXIT_SUCCESS;
}

int calc_ncols_from_rank(int rank, int size, int nx)
{
  int ncols;

  ncols = nx / size;       /* integer division */
  if ((nx % size) != 0) {  /* if there is a remainder */
    if (rank == size - 1)
      ncols += nx % size;  /* add remainder to last rank */
  }

  return ncols;
}

int timestep(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  accelerate_flow(params, cells, obstacles);
  propagate(params, cells, tmp_cells);
  rebound(params, cells, tmp_cells, obstacles);
  collision(params, cells, tmp_cells, obstacles);
  return EXIT_SUCCESS;
}

int accelerate_flow(const t_param params, t_speed* cells, int* obstacles)
{
  /* compute weighting factors */
  float w1 = params.density * params.accel / 9.f;
  float w2 = params.density * params.accel / 36.f;

  /* modify the 2nd row of the grid */
  int jj = params.ny - 2 -1; // acount for halo row with -1

  for (int ii = 0; ii < params.nx; ii++)
  {
    /* if the cell is not occupied and
    ** we don't send a negative density */
    if (!obstacles[ii + jj*params.nx]
        && (cells[ii + jj*params.nx].speeds[3] - w1) > 0.f
        && (cells[ii + jj*params.nx].speeds[6] - w2) > 0.f
        && (cells[ii + jj*params.nx].speeds[7] - w2) > 0.f)
    {
      /* increase 'east-side' densities */
      cells[ii + jj*params.nx].speeds[1] += w1;
      cells[ii + jj*params.nx].speeds[5] += w2;
      cells[ii + jj*params.nx].speeds[8] += w2;
      /* decrease 'west-side' densities */
      cells[ii + jj*params.nx].speeds[3] -= w1;
      cells[ii + jj*params.nx].speeds[6] -= w2;
      cells[ii + jj*params.nx].speeds[7] -= w2;
    }
  }

  return EXIT_SUCCESS;
}

int propagate(const t_param params, t_speed* cells, t_speed* tmp_cells)
{
  /* loop over _all_ cells */
  for (int jj = 1; jj < params.ny-1; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* determine indices of axis-direction neighbours
      ** respecting periodic boundary conditions (wrap around) */
      int y_n = (jj + 1) % params.ny;
      int x_e = (ii + 1) % params.nx;
      int y_s = (jj == 0) ? (jj + params.ny - 1) : (jj - 1);
      int x_w = (ii == 0) ? (ii + params.nx - 1) : (ii - 1);
      /* propagate densities from neighbouring cells, following
      ** appropriate directions of travel and writing into
      ** scratch space grid */
      tmp_cells[ii + jj*params.nx].speeds[0] = cells[ii + jj*params.nx].speeds[0]; /* central cell, no movement */
      tmp_cells[ii + jj*params.nx].speeds[1] = cells[x_w + jj*params.nx].speeds[1]; /* east */
      tmp_cells[ii + jj*params.nx].speeds[2] = cells[ii + y_s*params.nx].speeds[2]; /* north */
      tmp_cells[ii + jj*params.nx].speeds[3] = cells[x_e + jj*params.nx].speeds[3]; /* west */
      tmp_cells[ii + jj*params.nx].speeds[4] = cells[ii + y_n*params.nx].speeds[4]; /* south */
      tmp_cells[ii + jj*params.nx].speeds[5] = cells[x_w + y_s*params.nx].speeds[5]; /* north-east */
      tmp_cells[ii + jj*params.nx].speeds[6] = cells[x_e + y_s*params.nx].speeds[6]; /* north-west */
      tmp_cells[ii + jj*params.nx].speeds[7] = cells[x_e + y_n*params.nx].speeds[7]; /* south-west */
      tmp_cells[ii + jj*params.nx].speeds[8] = cells[x_w + y_n*params.nx].speeds[8]; /* south-east */
    }
  }

  return EXIT_SUCCESS;
}

int rebound(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  /* loop over the cells in the grid */
  for (int jj = 1; jj < params.ny-1; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* if the cell contains an obstacle */
      if (obstacles[jj*params.nx + ii])
      {
        /* called after propagate, so taking values from scratch space
        ** mirroring, and writing into main grid */
        cells[ii + jj*params.nx].speeds[1] = tmp_cells[ii + jj*params.nx].speeds[3];
        cells[ii + jj*params.nx].speeds[2] = tmp_cells[ii + jj*params.nx].speeds[4];
        cells[ii + jj*params.nx].speeds[3] = tmp_cells[ii + jj*params.nx].speeds[1];
        cells[ii + jj*params.nx].speeds[4] = tmp_cells[ii + jj*params.nx].speeds[2];
        cells[ii + jj*params.nx].speeds[5] = tmp_cells[ii + jj*params.nx].speeds[7];
        cells[ii + jj*params.nx].speeds[6] = tmp_cells[ii + jj*params.nx].speeds[8];
        cells[ii + jj*params.nx].speeds[7] = tmp_cells[ii + jj*params.nx].speeds[5];
        cells[ii + jj*params.nx].speeds[8] = tmp_cells[ii + jj*params.nx].speeds[6];
      }
    }
  }

  return EXIT_SUCCESS;
}

void test_vels(const char* output_file, double *vels, int steps) {
  FILE* fp = fopen(output_file, "w");
  for(int i = 0; i < steps; ++i) {
    double vel = vels[i];
    fprintf(fp, "%.12lf\n", vel);
  }

  fclose(fp);
}

void output_state(const char* output_file, int step, t_speed *cells, int *obstacles, int nx, int ny) {
  FILE* fp = fopen(output_file, "a");
  if (fp == NULL)
  {
    printf("could not open input parameter file: %s", output_file);
    return;
  }
  fprintf(fp, "Step %d:\n", step);
  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      for(int z = 0; z < 9; ++z) {
        fprintf(fp, "%f ", cells[i*nx + j].speeds[z]);
      }
      fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
  }
  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      fprintf(fp, "%d ", obstacles[i*nx + j]);
    }
    fprintf(fp, "\n");
  }
  fprintf(fp, "\n\n");
  fclose(fp);
}

int collision(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  const float c_sq = 1.f / 3.f; /* square of speed of sound */
  const float w0 = 4.f / 9.f;  /* weighting factor */
  const float w1 = 1.f / 9.f;  /* weighting factor */
  const float w2 = 1.f / 36.f; /* weighting factor */

  /* loop over the cells in the grid
  ** NB the collision step is called after
  ** the propagate step and so values of interest
  ** are in the scratch-space grid */
  for (int jj = 1; jj < params.ny-1; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* don't consider occupied cells */
      if (!obstacles[ii + jj*params.nx])
      {
        /* compute local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += tmp_cells[ii + jj*params.nx].speeds[kk];
        }

        /* compute x velocity component */
        float u_x = (tmp_cells[ii + jj*params.nx].speeds[1]
                      + tmp_cells[ii + jj*params.nx].speeds[5]
                      + tmp_cells[ii + jj*params.nx].speeds[8]
                      - (tmp_cells[ii + jj*params.nx].speeds[3]
                         + tmp_cells[ii + jj*params.nx].speeds[6]
                         + tmp_cells[ii + jj*params.nx].speeds[7]))
                     / local_density;
        /* compute y velocity component */
        float u_y = (tmp_cells[ii + jj*params.nx].speeds[2]
                      + tmp_cells[ii + jj*params.nx].speeds[5]
                      + tmp_cells[ii + jj*params.nx].speeds[6]
                      - (tmp_cells[ii + jj*params.nx].speeds[4]
                         + tmp_cells[ii + jj*params.nx].speeds[7]
                         + tmp_cells[ii + jj*params.nx].speeds[8]))
                     / local_density;

        /* velocity squared */
        float u_sq = u_x * u_x + u_y * u_y;

        /* directional velocity components */
        float u[NSPEEDS];
        u[1] =   u_x;        /* east */
        u[2] =         u_y;  /* north */
        u[3] = - u_x;        /* west */
        u[4] =       - u_y;  /* south */
        u[5] =   u_x + u_y;  /* north-east */
        u[6] = - u_x + u_y;  /* north-west */
        u[7] = - u_x - u_y;  /* south-west */
        u[8] =   u_x - u_y;  /* south-east */

        /* equilibrium densities */
        float d_equ[NSPEEDS];
        /* zero velocity density: weight w0 */
        d_equ[0] = w0 * local_density
                   * (1.f - u_sq / (2.f * c_sq));
        /* axis speeds: weight w1 */
        d_equ[1] = w1 * local_density * (1.f + u[1] / c_sq
                                         + (u[1] * u[1]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[2] = w1 * local_density * (1.f + u[2] / c_sq
                                         + (u[2] * u[2]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[3] = w1 * local_density * (1.f + u[3] / c_sq
                                         + (u[3] * u[3]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[4] = w1 * local_density * (1.f + u[4] / c_sq
                                         + (u[4] * u[4]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        /* diagonal speeds: weight w2 */
        d_equ[5] = w2 * local_density * (1.f + u[5] / c_sq
                                         + (u[5] * u[5]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[6] = w2 * local_density * (1.f + u[6] / c_sq
                                         + (u[6] * u[6]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[7] = w2 * local_density * (1.f + u[7] / c_sq
                                         + (u[7] * u[7]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[8] = w2 * local_density * (1.f + u[8] / c_sq
                                         + (u[8] * u[8]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));

        /* relaxation step */
        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          cells[ii + jj*params.nx].speeds[kk] = tmp_cells[ii + jj*params.nx].speeds[kk]
                                                  + params.omega
                                                  * (d_equ[kk] - tmp_cells[ii + jj*params.nx].speeds[kk]);
        }
      }
    }
  }

  return EXIT_SUCCESS;
}

int test_files(const char* file1, const char* file2, int nx, int ny, t_speed *cells, int *obstacles) {
  FILE *fp1 = fopen(file1, "r");
  FILE *fp2 = fopen(file2, "r");

  if (fp1 == NULL)
  {
    printf("could not open input parameter file: %s", file1);
    return 0;
  }
  if (fp2 == NULL)
  {
    printf("could not open input parameter file: %s", file2);
    return 0;
  }

  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      for(int z = 0; z < NSPEEDS; ++z) {

        float f1, f2;
        int r1 = fscanf(fp1, "%f", &f1);
        int r2 = fscanf(fp2, "%f", &f2);
        if(r1 == 0 || r2 == 0) {
          printf("could not parse nums\n");
          return 0;
        }
        if(f1 != f2) {
          printf("DIFFERENT OBS VALS %f  %f\n", f1, f2);
          return 0;
          //die(message, __LINE__, __FILE__);
        }
      }

    }
  }


  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      int f1, f2;
      int r1 = fscanf(fp1, "%d", &f1);
      int r2 = fscanf(fp2, "%d", &f2);
      if(r1 == 0 || r2 == 0) {
        printf("could not parse nums\n");
        return 0;
      }
      if(f1 != f2) {
        printf("DIFFERENT OBS VALS %d  %d\n", f1, f2);
        return 0;
        //die(message, __LINE__, __FILE__);
      }
    }
  }

  fclose(fp1);
  fclose(fp2);
  return 1;
}

void test_run(const char* output_file, int nx, int ny, t_speed *cells, int *obstacles) {
  FILE* fp = fopen(output_file, "w");
  char message[1024];

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", output_file);
    die(message, __LINE__, __FILE__);
  }

  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      for(int z = 0; z < NSPEEDS; ++z) {
        fprintf(fp, "%f ", cells[i*nx + j].speeds[z]);
      }

    }
  }
  fprintf(fp, "\n\n");

  for(int i = 0; i < ny; ++i) {
    for(int j = 0; j < nx; ++j) {
      fprintf(fp, "%d ", obstacles[i*nx + j]);
    }
  }

  fclose(fp);
}

double av_velocity(const t_param params, t_speed* cells, int* obstacles)
{
  int    tot_cells = 0;  /* no. of cells used in calculation */
  double tot_u;          /* accumulated magnitudes of velocity for each cell */

  /* initialise */
  tot_u = 0.f;

  /* loop over all non-blocked cells */
  int skipped = 0;
  //printf("START PRINTING VELOCITIES:\n");
  for (int jj = 1; jj < params.ny-1; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* ignore occupied cells */
      if (!obstacles[ii + jj*params.nx])
      {
        /* local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* x-component of velocity */
        float u_x = (cells[ii + jj*params.nx].speeds[1]
                      + cells[ii + jj*params.nx].speeds[5]
                      + cells[ii + jj*params.nx].speeds[8]
                      - (cells[ii + jj*params.nx].speeds[3]
                         + cells[ii + jj*params.nx].speeds[6]
                         + cells[ii + jj*params.nx].speeds[7]))
                     / local_density;
        /* compute y velocity component */
        float u_y = (cells[ii + jj*params.nx].speeds[2]
                      + cells[ii + jj*params.nx].speeds[5]
                      + cells[ii + jj*params.nx].speeds[6]
                      - (cells[ii + jj*params.nx].speeds[4]
                         + cells[ii + jj*params.nx].speeds[7]
                         + cells[ii + jj*params.nx].speeds[8]))
                     / local_density;
        /* accumulate the norm of x- and y- velocity components */
        //double to_add = cells[ii + jj*params.nx].speeds[1];
        //u_x + u_y;
        //printf("ii: %d, jj: %d, val: %f\n", ii, jj, to_add);
        tot_u += sqrtf(10000*((u_x * u_x) + (u_y * u_y)));
        //tot_u += to_add;
        /* increase counter of inspected cells */
        ++tot_cells;
      } else ++skipped;
    }
    //printf("skipped: %d\n", skipped);
  }
  //printf("DONE PRINTING VELOCITIES, total: %f\n", tot_u);
  //printf("total cells: %d\n", (int) tot_cells);

  return tot_u;
}

void initialise_params_from_file(const char* paramfile, t_param* params) {
  char   message[1024];  /* message buffer */
  int    retval;         /* to hold return value for checking */
  FILE* fp;

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }

  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1) die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1) die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1) die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);
}

int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr)
{
  char   message[1024];  /* message buffer */
  FILE*   fp;            /* file pointer */
  int    xx, yy;         /* generic array indices */
  int    blocked;        /* indicates whether a cell is blocked by an obstacle */
  int    retval;         /* to hold return value for checking */

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }

  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1) die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1) die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1) die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);

  /*
  ** Allocate memory.
  **
  ** Remember C is pass-by-value, so we need to
  ** pass pointers into the initialise function.
  **
  ** NB we are allocating a 1D array, so that the
  ** memory will be contiguous.  We still want to
  ** index this memory as if it were a (row major
  ** ordered) 2D array, however.  We will perform
  ** some arithmetic using the row and column
  ** coordinates, inside the square brackets, when
  ** we want to access elements of this array.
  **
  ** Note also that we are using a structure to
  ** hold an array of 'speeds'.  We will allocate
  ** a 1D array of these structs.
  */

  /* main grid */
  *cells_ptr = (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*cells_ptr == NULL) die("cannot allocate memory for cells", __LINE__, __FILE__);

  /* 'helper' grid, used as scratch space */
  *tmp_cells_ptr = (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*tmp_cells_ptr == NULL) die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  *obstacles_ptr = malloc(sizeof(int) * (params->ny * params->nx));

  if (*obstacles_ptr == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* initialise densities */
  float w0 = params->density * 4.f / 9.f;
  float w1 = params->density      / 9.f;
  float w2 = params->density      / 36.f;

  for (int jj = 0; jj < params->ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      /* centre */
      (*cells_ptr)[ii + jj*params->nx].speeds[0] = w0;
      /* axis directions */
      (*cells_ptr)[ii + jj*params->nx].speeds[1] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[2] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[3] = w1;
      (*cells_ptr)[ii + jj*params->nx].speeds[4] = w1;
      /* diagonals */
      (*cells_ptr)[ii + jj*params->nx].speeds[5] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[6] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[7] = w2;
      (*cells_ptr)[ii + jj*params->nx].speeds[8] = w2;
    }
  }

  /* first set all cells in obstacle array to zero */
  for (int jj = 0; jj < params->ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      (*obstacles_ptr)[ii + jj*params->nx] = 0;
    }
  }

  /* open the obstacle data file */
  fp = fopen(obstaclefile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input obstacles file: %s", obstaclefile);
    die(message, __LINE__, __FILE__);
  }

  /* read-in the blocked cells list */
  while ((retval = fscanf(fp, "%d %d %d\n", &xx, &yy, &blocked)) != EOF)
  {
    /* some checks */
    if (retval != 3) die("expected 3 values per line in obstacle file", __LINE__, __FILE__);

    if (xx < 0 || xx > params->nx - 1) die("obstacle x-coord out of range", __LINE__, __FILE__);

    if (yy < 0 || yy > params->ny - 1) die("obstacle y-coord out of range", __LINE__, __FILE__);

    if (blocked != 1) die("obstacle blocked value should be 1", __LINE__, __FILE__);

    /* assign to array */
    (*obstacles_ptr)[xx + yy*params->nx] = blocked;
  }

  /* and close the file */
  fclose(fp);

  return EXIT_SUCCESS;
}

int finalise(const t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
             int** obstacles_ptr, double** av_vels_ptr)
{
  /*
  ** free up allocated memory
  */
  free(*cells_ptr);
  *cells_ptr = NULL;

  free(*tmp_cells_ptr);
  *tmp_cells_ptr = NULL;

  free(*obstacles_ptr);
  *obstacles_ptr = NULL;

  free(*av_vels_ptr);
  *av_vels_ptr = NULL;

  return EXIT_SUCCESS;
}


float calc_reynolds(const t_param params, t_speed* cells, int* obstacles)
{
  const float viscosity = 1.f / 6.f * (2.f / params.omega - 1.f);

  return av_velocity(params, cells, obstacles) * params.reynolds_dim / viscosity;
}

float total_density(const t_param params, t_speed* cells)
{
  float total = 0.f;  /* accumulator */

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      for (int kk = 0; kk < NSPEEDS; kk++)
      {
        total += cells[ii + jj*params.nx].speeds[kk];
      }
    }
  }

  return total;
}

int write_values(const t_param params, t_speed* cells, int* obstacles, double* av_vels)
{
  FILE* fp;                     /* file pointer */
  const float c_sq = 1.f / 3.f; /* sq. of speed of sound */
  float local_density;         /* per grid cell sum of densities */
  float pressure;              /* fluid pressure in grid cell */
  float u_x;                   /* x-component of velocity in grid cell */
  float u_y;                   /* y-component of velocity in grid cell */
  float u;                     /* norm--root of summed squares--of u_x and u_y */

  fp = fopen(FINALSTATEFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* an occupied cell */
      if (obstacles[ii + jj*params.nx])
      {
        u_x = u_y = u = 0.f;
        pressure = params.density * c_sq;
      }
      /* no obstacle */
      else
      {
        local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* compute x velocity component */
        u_x = (cells[ii + jj*params.nx].speeds[1]
               + cells[ii + jj*params.nx].speeds[5]
               + cells[ii + jj*params.nx].speeds[8]
               - (cells[ii + jj*params.nx].speeds[3]
                  + cells[ii + jj*params.nx].speeds[6]
                  + cells[ii + jj*params.nx].speeds[7]))
              / local_density;
        /* compute y velocity component */
        u_y = (cells[ii + jj*params.nx].speeds[2]
               + cells[ii + jj*params.nx].speeds[5]
               + cells[ii + jj*params.nx].speeds[6]
               - (cells[ii + jj*params.nx].speeds[4]
                  + cells[ii + jj*params.nx].speeds[7]
                  + cells[ii + jj*params.nx].speeds[8]))
              / local_density;
        /* compute norm of velocity */
        u = sqrtf((u_x * u_x) + (u_y * u_y));
        /* compute pressure */
        pressure = local_density * c_sq;
      }

      /* write to file */
      fprintf(fp, "%d %d %.12E %.12E %.12E %.12E %d\n", ii, jj, u_x, u_y, u, pressure, obstacles[ii * params.nx + jj]);
    }
  }

  fclose(fp);

  fp = fopen(AVVELSFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.maxIters; ii++)
  {
    fprintf(fp, "%d:\t%.12E\n", ii, av_vels[ii]);
  }

  fclose(fp);

  return EXIT_SUCCESS;
}

void die(const char* message, const int line, const char* file)
{
  fprintf(stderr, "Error at line %d of file %s:\n", line, file);
  fprintf(stderr, "%s\n", message);
  fflush(stderr);
  exit(EXIT_FAILURE);
}

void usage(const char* exe)
{
  fprintf(stderr, "Usage: %s <paramfile> <obstaclefile>\n", exe);
  exit(EXIT_FAILURE);
}
