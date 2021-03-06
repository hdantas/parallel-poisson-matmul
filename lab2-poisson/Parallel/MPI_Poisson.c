/*
 * SEQ_Poisson.c
 * 2D Poison equation solver
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "mpi.h"

#define DEBUG 0
#define OMEGA 1.95

#define max(a,b) ((a)>(b)?a:b)

enum
{
  X_DIR, Y_DIR
};

/* global variables */
int gridsize[2];
double precision_goal;    /* precision_goal of solution */
int max_iter;     /* maximum number of iterations alowed */
double global_delta;
MPI_Datatype border_type[2];

/* benchmark related variables */
clock_t ticks;      /* number of systemticks */
int timer_on = 0;   /* is timer running? */

/* local grid related variables */
double **phi;   /* grid */
int **source;   /* TRUE if subgrid element is a source */
int dim[2];     /* grid dimensions */

double wtime; /* wallclock time */

/* process specific variables */
int proc_rank;                  /* rank of current process */
int proc_coord[2];              /* coordinates of current process in processgrid */
int proc_top, proc_right, proc_bottom, proc_left; /* ranks of neigboring procs */

int P;                          /* total number of processes */
int P_grid[2];                  /* processgrid dimensions */
MPI_Comm grid_comm;             /* grid COMMUNICATOR */
MPI_Status status;

int offset[2];
long data_communicated;

void Setup_Grid();
void Setup_Proc_Grid(int argc, char **argv);
double Do_Step(int parity);
void Solve();
void Write_Grid();
void Clean_Up();
void Debug(char *mesg, int terminate);
void start_timer();
void resume_timer();
void stop_timer();
void print_timer();
void Setup_MPI_Datatypes();
void Exchange_Borders();


void start_timer()
{
  if (!timer_on)
  {
    MPI_Barrier(grid_comm);
    ticks = clock();
    wtime = MPI_Wtime();
    timer_on = 1;
  }
}

void resume_timer()
{
  if (!timer_on)
  {
    ticks = clock() - ticks;
    wtime = MPI_Wtime() - wtime;
    timer_on = 1;    
  }
}

void stop_timer()
{
  if (timer_on)
  {
    ticks = clock() - ticks;
    wtime = MPI_Wtime() - wtime;
    timer_on = 0;
  }
}

void print_timer()
{
  if (timer_on)
  {
    stop_timer();
    printf("(%i) Elapsed Wtime: %14.6f s (%5.1f%% CPU)\n", proc_rank, wtime, 100.0 * ticks * (1.0 / CLOCKS_PER_SEC) / wtime);
    resume_timer();    
  }
  else
    printf("(%i) Elapsed Wtime: %14.6f s (%5.1f%% CPU)\n", proc_rank, wtime, 100.0 * ticks * (1.0 / CLOCKS_PER_SEC) / wtime);
}


void Debug(char *mesg, int terminate)
{
  if (DEBUG || terminate)
    printf("(%i)\t%s\n", proc_rank, mesg);
  if (terminate)
    exit(1);
}

void Setup_Grid()
{
  int x, y, s;
  int upper_offset[2];
  double source_x, source_y, source_val;
  FILE *f;

  Debug("Setup_Subgrid", 0);

  if (proc_rank == 0) /* only process 0 may execute this if */
  {
    f = fopen("input.dat", "r");
    if (f == NULL)
      Debug("Error opening input.dat", 1);
    fscanf(f, "nx: %i\n", &gridsize[X_DIR]);
    fscanf(f, "ny: %i\n", &gridsize[Y_DIR]);
    fscanf(f, "precision goal: %lf\n", &precision_goal);
    fscanf(f, "max iterations: %i\n", &max_iter);
  }

  MPI_Bcast(&gridsize, 2, MPI_INT, 0, grid_comm); /* broadcast the array gridsize in one call */
  MPI_Bcast(&precision_goal, 1, MPI_DOUBLE, 0, grid_comm); /* broadcast precision_goal */
  MPI_Bcast(&max_iter, 1, MPI_INT, 0, grid_comm); /* broadcast max_iter */  
  
  if (proc_rank == 0){
    // printf("OMEGA = %1.2lf\n", OMEGA);
    printf("g: Grid Size = %d x %d\n", gridsize[X_DIR], gridsize[Y_DIR]);
  }

  if (DEBUG) {
    printf("(%d) precision_goal = %lf\tmax_iter = %d\n", proc_rank, precision_goal, max_iter);
  }


  /* Calculate top left corner coordinates of local grid */
  offset[X_DIR] = gridsize[X_DIR] * proc_coord[X_DIR] / P_grid[X_DIR];
  offset[Y_DIR] = gridsize[Y_DIR] * proc_coord[Y_DIR] / P_grid[Y_DIR];
  upper_offset[X_DIR] = gridsize[X_DIR] * (proc_coord[X_DIR] + 1) / P_grid[X_DIR];
  upper_offset[Y_DIR] = gridsize[Y_DIR] * (proc_coord[Y_DIR] + 1) / P_grid[Y_DIR];

  /* Calculate dimensions of local grid */
  dim[Y_DIR] = upper_offset[Y_DIR] - offset[Y_DIR];
  dim[X_DIR] = upper_offset[X_DIR] - offset[X_DIR];

  /* Add space for rows/columns of neighboring grid */
  dim[Y_DIR] += 2;
  dim[X_DIR] += 2;
  
  if (DEBUG)
    printf("(%d) dim[X_DIR] = %d\tdim[Y_DIR] = %d\n", proc_rank, dim[X_DIR], dim[Y_DIR]);


  /* allocate memory */
  if ((phi = malloc(dim[X_DIR] * sizeof(*phi))) == NULL)
    Debug("Setup_Subgrid : malloc(phi) failed", 1);
  if ((source = malloc(dim[X_DIR] * sizeof(*source))) == NULL)
    Debug("Setup_Subgrid : malloc(source) failed", 1);
  if ((phi[0] = malloc(dim[Y_DIR] * dim[X_DIR] * sizeof(**phi))) == NULL)
    Debug("Setup_Subgrid : malloc(*phi) failed", 1);
  if ((source[0] = malloc(dim[Y_DIR] * dim[X_DIR] * sizeof(**source))) == NULL)
    Debug("Setup_Subgrid : malloc(*source) failed", 1);
  for (x = 1; x < dim[X_DIR]; x++)
  {
    phi[x] = phi[0] + x * dim[Y_DIR];
    source[x] = source[0] + x * dim[Y_DIR];
  }

  /* set all values to '0' */
  for (x = 0; x < dim[X_DIR]; x++)
    for (y = 0; y < dim[Y_DIR]; y++)
    {
      phi[x][y] = 0.0;
      source[x][y] = 0;
    }

  /* put sources in field */
  do {
    if (proc_rank == 0) /* only process 0 may scan next line of input */
      s = fscanf(f, "source: %lf %lf %lf\n", &source_x, &source_y, &source_val);
    
    MPI_Bcast(&s, 1, MPI_INT, 0, grid_comm); /* The return value of this scan is broadcast even though it is no input data */
    
    if (s == 3)
    {
      MPI_Bcast(&source_x  , 1, MPI_DOUBLE, 0, grid_comm); /* broadcast source_x */
      MPI_Bcast(&source_y  , 1, MPI_DOUBLE, 0, grid_comm); /* broadcast source_y */
      MPI_Bcast(&source_val, 1, MPI_DOUBLE, 0, grid_comm); /* broadcast source_val */
      if (DEBUG)
        printf("(%d) source_x = %1.2lf, source_y = %1.2lf, source_val = %1.2lf\n", proc_rank, source_x, source_y, source_val);

      x = gridsize[X_DIR] * source_x;
      y = gridsize[Y_DIR] * source_y;
      x += 1;
      y += 1;

      x = x - offset[X_DIR];
      y = y - offset[Y_DIR];

      if (DEBUG) {
        printf("(%d) x = %d, dim[X_DIR] = %d\n", proc_rank, x, dim[X_DIR]);
        printf("(%d) y = %d, dim[Y_DIR] = %d\n", proc_rank, y, dim[Y_DIR]);
      }

      if ( x > 0 && x < dim[X_DIR] -1 && y > 0 && y < dim[Y_DIR] - 1)
      { /* indices in domain of this process */
        phi[x][y] = source_val;
        source[x][y] = 1;
      }
    }
  } while (s == 3);

  if (proc_rank == 0) /* only process 0 may close the file */
    fclose(f);
}

double Do_Step(int parity)
{
  int x, y;
  double old_phi;
  double c = 0.0;
  double max_err = 0.0;
  int aux = 0;
  /* calculate interior of grid */
  for (x = 1; x < dim[X_DIR] - 1; x++) {
      // for (y = 1; y < dim[Y_DIR] - 1; y++)
      //   if ((x + offset[X_DIR] + y + offset[Y_DIR]) % 2 == parity && source[x][y] != 1) /* use global coordinates for parity */
      aux = (x + offset[X_DIR] + offset[Y_DIR] + parity + 1) % 2;
      for (y = 1 + aux; y < dim[Y_DIR] - 1; y += 2)  
        if (source[x][y] != 1)
        {
          old_phi = phi[x][y];
          c = (phi[x + 1][y] + phi[x - 1][y] + phi[x][y + 1] + phi[x][y - 1]) * 0.25 - old_phi;
          phi[x][y] = old_phi + OMEGA * c;
          if (max_err < fabs(old_phi - phi[x][y]))
            max_err = fabs(old_phi - phi[x][y]);
        }
  }
  return max_err;
}

void Solve()
{
  int count = 0;
  double delta;
  double delta1, delta2;
  Debug("Solve", 0);

  /* give global_delta a higher value then precision_goal */
  global_delta = 2 * precision_goal;

  while (global_delta > precision_goal && count < max_iter)
  {
    // Debug("Do_Step 0", 0);
    delta1 = Do_Step(0);
    Exchange_Borders();

    // Debug("Do_Step 1", 0);
    delta2 = Do_Step(1);
    Exchange_Borders();

    delta = max(delta1, delta2);
    MPI_Allreduce(&delta, &global_delta, 1, MPI_DOUBLE, MPI_MAX, grid_comm);
    
    if (DEBUG)
      printf("delta = %lf\tglobal_delta = %lf\n", delta, global_delta);

    count++;
  }
  
  printf("(%i) n: Number of iterations : %i\n", proc_rank, count);
}

void Write_Grid()
{
  int x, y;
  FILE *f;
  char filename[40];

  sprintf(filename, "output%i.dat", proc_rank);

  if ((f = fopen(filename, "w")) == NULL)
    Debug("Write_Grid : fopen failed", 1);

  Debug("Write_Grid", 0);

  for (x = 1; x < dim[X_DIR] - 1; x++)
    for (y = 1; y < dim[Y_DIR] - 1; y++)
      fprintf(f, "%i %i %f\n", x + offset[X_DIR], y + offset[Y_DIR], phi[x][y]);

  fclose(f);
}

void Clean_Up()
{
  Debug("Clean_Up", 0);

  free(phi[0]);
  free(phi);
  free(source[0]);
  free(source);
}

void Setup_Proc_Grid(int argc, char **argv)
{
  int wrap_around[2];
  int reorder;

  Debug("My_MPI_Init", 0);

  /* Retrieve the number of processes P */
  MPI_Comm_size(MPI_COMM_WORLD, &P);

  /* Calculate the number of processes per column and per row for the grid */
  if (argc > 2)
  {
    P_grid[X_DIR] = atoi(argv[1]);
    P_grid[Y_DIR] = atoi(argv[2]);
    if (P_grid[X_DIR] * P_grid[Y_DIR] != P)
      Debug("ERROR : Process grid dimensions do not match with P", 1);
  }
  else
    Debug("ERROR : Wrong parameter input", 1);

  /* Create process topology (2D grid) */
  wrap_around[X_DIR] = 0;
  wrap_around[Y_DIR] = 0;       /* do not connect first and last process */
  reorder = 1;                  /* reorder process ranks */
  MPI_Cart_create(MPI_COMM_WORLD, 2, P_grid, wrap_around, reorder, &grid_comm);

  /* Retrieve new rank and carthesian coordinates of this process */
  MPI_Comm_rank(grid_comm, &proc_rank); /* Rank of process in new communicator */
  MPI_Cart_coords(grid_comm, proc_rank, 2, proc_coord); /* Coordinates of process in new communicator */

  if (proc_rank == 0)
    printf("pt: Topology: %d %d %d\n", P, P_grid[X_DIR], P_grid[Y_DIR]);

  if (DEBUG)
    printf("(%i) (x,y)=(%i,%i)\n", proc_rank, proc_coord[X_DIR], proc_coord[Y_DIR]);

  /* calculate ranks of neighbouring processes */
  MPI_Cart_shift(grid_comm, Y_DIR, 1, &proc_top, &proc_bottom); /* rank of processes proc_top and proc_bottom */
  MPI_Cart_shift(grid_comm, X_DIR, 1, &proc_left, &proc_right); /* rank of processes proc_left and proc_right */

  if (DEBUG)
    printf("(%i) top %i, right %i, bottom %i, left %i\n", proc_rank, proc_top, proc_right, proc_bottom, proc_left);
}

void Setup_MPI_Datatypes()
{
  Debug("Setup_MPI_Datatypes", 0);
 
  /* Datatype for vertical data exchange (Y_DIR) */
  MPI_Type_vector(dim[X_DIR] - 2, 1, dim[Y_DIR], MPI_DOUBLE, &border_type[Y_DIR]);
  MPI_Type_commit(&border_type[Y_DIR]);
  
  /* Datatype for horizontal data exchange (X_DIR) */
  MPI_Type_vector(dim[Y_DIR] - 2, 1, 1, MPI_DOUBLE, &border_type[X_DIR]);
  MPI_Type_commit(&border_type[X_DIR]);
}

void Exchange_Borders()
{
  Debug("Exchange_Borders", 0);

  resume_timer();
  // all traffic in direction "top"
  MPI_Sendrecv( &phi[1][1],              // sendbuf   - initial address of send buffer (choice)
                1,                       // sendcount - number of elements in send buffer (integer)
                border_type[Y_DIR],      // sendtype  - type of elements in send buffer (handle)
                proc_top,                // dest      - rank of destination (integer)
                0,                       // sendtag   - send tag (integer)
                &phi[1][dim[Y_DIR] - 1], // recvbuf   - initial address of receive buffer (choice)
                1,                       // recvcount - number of elements in receive buffer (integer)
                border_type[Y_DIR],      // recvtype  - type of elements in receive buffer (handle)
                proc_bottom,             // source    - rank of source (integer)
                0,                       // recvtag   - receive tag (integer)
                grid_comm,               // comm      - communicator (handle)
                &status);                // status    - status object (Status).  This refers to the receive operation.


  // all traffic in direction "bottom"
  MPI_Sendrecv( &phi[1][dim[Y_DIR] - 2], // sendbuf   - initial address of send buffer (choice)
                1,                       // sendcount - number of elements in send buffer (integer)
                border_type[Y_DIR],      // sendtype  - type of elements in send buffer (handle)
                proc_bottom,             // dest      - rank of destination (integer)
                0,                       // sendtag   - send tag (integer)
                &phi[1][0],              // recvbuf   - initial address of receive buffer (choice)
                1,                       // recvcount - number of elements in receive buffer (integer)
                border_type[Y_DIR],      // recvtype  - type of elements in receive buffer (handle)
                proc_top,                // source    - rank of source (integer)
                0,                       // recvtag   - receive tag (integer)
                grid_comm,               // comm      - communicator (handle)
                &status);                // status    - status object (Status).  This refers to the receive operation.

  // all traffic in direction "left"
  MPI_Sendrecv( &phi[1][1],              // sendbuf   - initial address of send buffer (choice)
                1,                       // sendcount - number of elements in send buffer (integer)
                border_type[X_DIR],      // sendtype  - type of elements in send buffer (handle)
                proc_left,               // dest      - rank of destination (integer)
                0,                       // sendtag   - send tag (integer)
                &phi[dim[X_DIR] - 1][1], // recvbuf   - initial address of receive buffer (choice)
                1,                       // recvcount - number of elements in receive buffer (integer)
                border_type[X_DIR],      // recvtype  - type of elements in receive buffer (handle)
                proc_right,              // source    - rank of source (integer)
                0,                       // recvtag   - receive tag (integer)
                grid_comm,               // comm      - communicator (handle)
                &status);                // status    - status object (Status).  This refers to the receive operation.
  
  // all traffic in direction "right"
  MPI_Sendrecv( &phi[dim[X_DIR] - 2][1], // sendbuf   - initial address of send buffer (choice)
                1,                       // sendcount - number of elements in send buffer (integer)
                border_type[X_DIR],      // sendtype  - type of elements in send buffer (handle)
                proc_right,              // dest      - rank of destination (integer)
                0,                       // sendtag   - send tag (integer)
                &phi[0][1],              // recvbuf   - initial address of receive buffer (choice)
                1,                       // recvcount - number of elements in receive buffer (integer)
                border_type[X_DIR],      // recvtype  - type of elements in receive buffer (handle)
                proc_left,               // source    - rank of source (integer)
                0,                       // recvtag   - receive tag (integer)
                grid_comm,               // comm      - communicator (handle)
                &status);                // status    - status object (Status).  This refers to the receive operation.
  stop_timer();
  data_communicated += 2 * dim[X_DIR] + 2 * dim[Y_DIR] - 8;
}


int main(int argc, char **argv)
{
  int size_mpi_double;
  long data_communicated_bytes;
  long global_communicated_bytes = 0;

  MPI_Init(&argc, &argv);
   
  Setup_Proc_Grid(argc, argv);

  // start_timer();

  Setup_Grid();

  Setup_MPI_Datatypes();

  Solve();

  Write_Grid();

  print_timer();

  MPI_Type_size(MPI_DOUBLE, &size_mpi_double);
  data_communicated_bytes = data_communicated * size_mpi_double;
  MPI_Reduce(&data_communicated_bytes, &global_communicated_bytes, 1, MPI_LONG, MPI_SUM, 0, grid_comm);

  if (proc_rank == 0) {
    printf("ds: Data sent = %ld\n", global_communicated_bytes);
  }

  Clean_Up();

  MPI_Finalize();

  return 0;
}
