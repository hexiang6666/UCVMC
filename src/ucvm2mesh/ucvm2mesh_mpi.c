/**
 * ucvm2mesh_mpi.c - Query UCVM to produce a 3D mesh on a regular grid
 *
 * Created by Patrick Small <patrices@usc.edu>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <getopt.h>
#include <mpi.h>
#include <unistd.h>
#include "um_dtypes.h"
#include "um_mpi.h"
#include "um_utils.h"
#include "um_stat.h"
#include "um_config.h"

void debugMe()
{
    int pause = 0;
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    fprintf(stdout,"PID %d on %s ready for attach\n", getpid(), hostname);
    fflush(stdout);
    while (0 == pause)
        sleep(5);
}

/* Get opt args */
extern char *optarg;
extern int optind, opterr, optopt;


/* Display usage information */
void usage(char *arg)
{
  printf("Usage: %s [-h] [-o dir] -f configfile\n\n", arg);

  printf("where:\n");
  printf("\t-h: help message\n");
  printf("\t-o: final stage out directory for mesh files\n");
  printf("\t-f: config file containing mesh params\n\n");
  printf("Config file format:\n");
  printf("\tucvmlist: comma-delimited list of CVMs to query (as supported by UCVM)\n");
  printf("\tucvmconf: UCVM API config file\n");
  printf("\tgridtype: location of x-y gridded points: VERTEX, or CENTER\n");
  printf("\tspacing: grid spacing (units appropriate for proj)\n");
  printf("\tproj: Proj.4 projection specification, or 'cmu' for TeraShake\n");
  printf("\trot: proj rotation angle in degrees, (+ is counter-clockwise)\n");
  printf("\tx0: longitude of origin (deg), or x offset in cmu proj (m)\n");
  printf("\ty0: latitude of origin (deg), or y offset in cmu proj (m)\n");
  printf("\tz0: depth of origin (m, typically 0.0)\n");
  printf("\tnx: number of points along x-axis\n");
  printf("\tny: number of points along y-axis\n");
  printf("\tnz: number of points along z-axis (depth positive)\n");
  printf("\tpx: number of procs along x-axis\n");
  printf("\tpy: number of procs along y-axis\n");
  printf("\tpz: number of procs along z-axis\n");
  printf("\tvp_min: vp minimum (m/s), enforced on vs_min conditions\n");
  printf("\tvs_min: vs minimum (m/s)\n");
  printf("\tmeshfile: path and basename to output mesh files\n");
  printf("\tgridfile: path and filename to output grid filesfiles\n");
  printf("\tmeshtype: mesh format: IJK-12, IJK-20, IJK-32, or SORD\n");
  printf("\tscratch: path to scratch space\n\n");

  printf("Version: %s\n\n", VERSION);
  return;
}


/* Initializer */
int init_app(int myid, int nproc, const char *cfgfile, mesh_config_t *cfg)
{
  /* Read in config */
  if (read_config(myid, nproc, cfgfile, cfg) != 0) {
    fprintf(stderr, "[%d] Failed to parse config file %s\n", myid, cfgfile);
    return(1);
  }

  if (myid == 0) {
    disp_config(cfg);
  }

  return(0);
}


/* Perform extraction from UCVM */
int extract(int myid,int mytask, int ntask, mesh_config_t *cfg, stat_t *stats) 
{
  fprintf(stdout,"[%d:%d] YYY calling extract\n",myid,mytask);
  fflush(stdout);

  /* Performance measurements */
  struct timeval start, end;
  double elapsed;

  /* Buffers */
  int num_grid, num_points;
  ucvm_point_t *pntbuf;
  ucvm_data_t *propbuf;
  mesh_ijk32_t *node_buf;

  int part_dims[3];
  int j, k, i_start, i_end, j_start, j_end, k_start, k_end, n;
  size_t grid_offset, pnt_offset;
  double z;
  FILE *ifp;

  /* Compute partition dims */
  part_dims[0] = cfg->dims.dim[0]/cfg->proc_dims.dim[0];
  part_dims[1] = cfg->dims.dim[1]/cfg->proc_dims.dim[1];
  part_dims[2] = cfg->dims.dim[2]/cfg->proc_dims.dim[2];

  /* Initialize statistics */
  memset(stats, 0, STAT_MAX_STATS*sizeof(stat_t));
  stats[STAT_MIN_VP].val = 100000.0;
  stats[STAT_MIN_VS].val = 100000.0;
  stats[STAT_MIN_RHO].val = 100000.0;
  stats[STAT_MIN_RATIO].val = 100000.0;

  /* Compute number of nodes in my partition of x-y grid  */
  num_grid = ((cfg->dims.dim[0]/cfg->proc_dims.dim[0]) * 
	       (cfg->dims.dim[1]/cfg->proc_dims.dim[1]));

  if (myid == 0) {
    fprintf(stdout, "[%d:%d] Opening output mesh file %s\n", 
	    myid,mytask, cfg->meshfile);
  }
  fprintf(stdout, "[%d:%d] before call mesh_open_mpi %d mytask out %d ntask %d num_grid\n", myid, mytask,  mytask, ntask, num_grid);
  fflush(stdout);
  if (mesh_open_mpi(mytask, ntask, \
		       &(cfg->dims), &(cfg->proc_dims),
		       cfg->meshfile, cfg->meshtype, num_grid) != 0) {
    fprintf(stderr, "[%d:%d] Error: mesh_open_mpi reported failure\n", myid,mytask);
    return(1);
  }

  /* Allocate buffers */
//  fprintf(stdout, "[%d:%d] Allocating %d grid points\n", myid, mytask, num_grid);
  pntbuf = malloc(num_grid * sizeof(ucvm_point_t));
  propbuf = malloc(num_grid * sizeof(ucvm_data_t));
  node_buf = malloc(num_grid * sizeof(mesh_ijk32_t));
  if ((pntbuf == NULL) || (propbuf == NULL) || (node_buf == NULL)) {
    fprintf(stderr, "[%d:%d] Failed to allocate buffers\n", myid,mytask);
    return(1);
  }

  /* Compute rank's local i,j,k range */
  k_start = ((int)(mytask / (cfg->proc_dims.dim[0] * cfg->proc_dims.dim[1]))
	     * part_dims[2]);
  k_end = k_start + part_dims[2];
  j_start = ((mytask % (cfg->proc_dims.dim[0] * cfg->proc_dims.dim[1])) 
	     / cfg->proc_dims.dim[0]) * part_dims[1];
  j_end = j_start + part_dims[1];
  i_start = ((mytask % (cfg->proc_dims.dim[0] * cfg->proc_dims.dim[1])) 
	     % cfg->proc_dims.dim[0]) * part_dims[0];
  i_end = i_start + part_dims[0];

//  fprintf(stdout,"[%d:%d] Partition dimensions: %d x %d x %d\n", myid, mytask,part_dims[0], part_dims[1], part_dims[2]);
//  fprintf(stdout,"[%d:%d] I,J,K start: %d, %d, %d\n", myid, mytask, i_start, j_start, k_start);
//  fprintf(stdout,"[%d:%d] I,J,K end: %d, %d, %d\n", myid, mytask, i_end, j_end, k_end);
//  fflush(stdout);

  /* Open the grid file */
  ifp = fopen(cfg->gridfile, "rb");
  if (ifp == NULL) {
    fprintf(stderr, "[%d:%d] Failed to open gridfile %s for reading\n", 
	    myid, mytask, cfg->gridfile);
    return(1);
  }

  /* Read grid partition for this rank */
  for (j = j_start; j < j_end; j++) {
    grid_offset = (j * cfg->dims.dim[0] + i_start) *sizeof(ucvm_point_t);
    pnt_offset = (j-j_start) * part_dims[0];
    fseek(ifp, grid_offset, SEEK_SET);
    if (fread(&(pntbuf[pnt_offset]), sizeof(ucvm_point_t), 
	      i_end-i_start, ifp) != i_end-i_start) {
      fprintf(stderr, "[%d:%d] Failed to read grid partition at j=%d\n", myid, mytask, j);
      return(1);
    }
  }

  /* Close grid file */
  fclose(ifp);

  /* For each k in k range, query UCVM */
 // if (myid == 0) {
  if (myid > 0) {
    fprintf(stdout, "[%d:%d] Starting extraction\n", myid, mytask);
  }
  num_points = 0;
  for (k = k_start; k < k_end; k++) {
    gettimeofday(&start,NULL);
    
    /* Set z coordinate */
    z = cfg->origin.coord[2] + (k * cfg->spacing);
    for (n = 0; n < num_grid; n++) {
      pntbuf[n].coord[2] = z;
    }

    /* Query UCVM at this k */
    if (ucvm_query(num_grid, pntbuf, propbuf) != UCVM_CODE_SUCCESS) {
      fprintf(stderr, "[%d:%d] Query UCVM failed\n", myid, mytask);
      return(1);
    }

    /* Convert the data points to a mesh node list */
    if (mesh_data_to_node(0, i_start, i_end, j_start, j_end,
			  k, pntbuf, propbuf, node_buf, cfg->vp_min,
			  cfg->vs_min) != 0) {
      return(1);
    }

    /* Calculate statistics */
    calc_stats_list(i_start, i_end, j_start, j_end, k, 
		    node_buf, stats);
    
    if (n != num_grid) {
      fprintf(stderr, "[%d:%d] Number of nodes mismatch\n", 
	      myid, mytask);
      return(1);
    }

    /* Calculate extraction elapsed time */
    gettimeofday(&end,NULL);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
      (end.tv_usec - start.tv_usec) / 1000.0;
/* XXX
    if (myid == 0) {
      fprintf(stdout,
	      "[%d:%d] Extracted slice %d (%d pnts) in %.2f ms, %f pps\n",
	      myid, mytask, k, num_grid, elapsed, 
	      (float)(num_grid/(elapsed/1000.0)));
      fflush(stdout);
    }
*/
    gettimeofday(&start,NULL);

    /* Write this buffer */
    if (mesh_write_mpi(&(node_buf[0]), num_grid) != 0) {
      fprintf(stderr, "[%d:%d] Failed to write nodes to mesh file\n", 
	      myid, mytask);
      return(1);
    }
    num_points = num_points + num_grid;
    fprintf(stdout, "[%d:%d] after mesh_write_mpi %d mytask out %d ntask %d num_points\n", myid, mytask,  mytask, ntask, num_points);

    /* Calculate write elapsed time */
    gettimeofday(&end,NULL);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
      (end.tv_usec - start.tv_usec) / 1000.0;
/* XXX
    if (myid == 0) {
      fprintf(stdout,
	      "[%d:%d] Wrote slice %d (%d pnts) in %.2f ms, %f pps\n",
	      myid, mytask,k, num_grid, elapsed, 
	      (float)(num_grid/(elapsed/1000.0)));
      fflush(stdout);
    }
*/
  }

//  fprintf(stdout, "[%d:%d] Extracted %d points\n", myid, mytask, num_points);
//  fflush(stdout);

  /* Close the mesh writer */
  mesh_close_mpi();
  fprintf(stdout, "[%d:%d] YYY done extraction\n", myid, mytask);
  fflush(stdout);

  /* Free buffers */
  free(pntbuf);
  free(propbuf);
  free(node_buf);

  return(0);
}


int main(int argc, char **argv)
{
  /* MPI stuff and distributed computation variables */
  int myid, nproc, pnlen;
  int ntask;
  int mytask;
  char procname[128];

  /* Config params */
  mesh_config_t cfg;
  size_t slice_size;

  /* Grid params */
  ucvm_projdef_t iproj, oproj;
  ucvm_trans_t trans;

  /* Filesytem IO */
  int i, j;
  char tmp[UCVM_MAX_PATH_LEN], tmp2[UCVM_MAX_PATH_LEN];

  /* Options */
  int opt;
  char configfile[UCVM_MAX_PATH_LEN], stageoutdir[UCVM_MAX_PATH_LEN];


  /* Init MPI */
  mpi_init(&argc, &argv, &nproc, &myid, procname, &pnlen);

  if (myid == 0) {
    printf("[%d] %s Version: %s\n", myid, argv[0],
	   VERSION);
    printf("[%d] Running on %d cores\n", myid, nproc);
    fflush(stdout);
  }




  /* Parse options */
  strcpy(stageoutdir, "");
  strcpy(configfile, "");
  while ((opt = getopt(argc, argv, "o:hf:")) != -1) {
    switch (opt) {
    case 'o':
      strcpy(stageoutdir, optarg);
      break;
    case 'f':
      strcpy(configfile, optarg);
      break;
    case 'h':
      usage(argv[0]);
      exit(0);
      break;
    default: /* '?' */
      fprintf(stderr, "[%d] Unrecognized option %c\n", myid, opt);
      usage(argv[0]);
      return(1);
    }
  }

  if (strcmp(configfile, "") == 0) {
    fprintf(stderr, "[%d] No config file specified\n", myid);
    return(1);
  }

  /* Application init */
  if (init_app(myid, nproc, configfile, &cfg) != 0) {
    fprintf(stderr, "[%d] Initialization failed\n", myid);
    return(1);
  }

  if (myid == 0) {

    printf("[%d] Initialization complete\n", myid);
    fflush(stdout);
    
    /* Delete output mesh file if present */
    deleteFile(cfg.meshfile);
    deleteFile(cfg.gridfile);

    /* Generate the 2D grid */
    sprintf(iproj.proj, "%s", UCVM_PROJ_GEO);
    sprintf(oproj.proj, "%s", cfg.proj);
    trans.rotate = cfg.rot;
    for (i = 0; i < 3; i++) {
      trans.origin[i] = cfg.origin.coord[i];
      trans.translate[i] = 0.0;
    }
    trans.gtype = cfg.gridtype;
    
    if (ucvm_grid_gen_file(&iproj, &trans, &oproj, &(cfg.dims), 
			   cfg.spacing, cfg.gridfile) != UCVM_CODE_SUCCESS) {
      fprintf(stderr, "[%d] Failed to create gridfile %s\n", 
	      myid, cfg.gridfile);
      return(1);
    }

    /* Convert grid from Proj.4 projection to latlong */
    printf("[%d] Converting grid to latlong\n", myid);
    fflush(stdout);
    slice_size = (size_t)cfg.dims.dim[0] * (size_t)cfg.dims.dim[1];
    if (ucvm_grid_convert_file(&oproj, &iproj, slice_size, 
			       cfg.gridfile) != UCVM_CODE_SUCCESS) {
      fprintf(stderr, "[%d] Failed to convert gridfile %s\n", 
	      myid, cfg.gridfile);
      return(1);
    }

    printf("[%d] Grid generation complete\n", myid);
    fflush(stdout);
  }

  /* Stagger each rank */
  mpi_barrier();
  sleep(myid % 120);

  if (myid == 0) {
    printf("[%d] Configuring UCVM\n", myid);
    fflush(stdout);
  }

  /* Setup UCVM */
  if (ucvm_init(cfg.ucvmconf) != UCVM_CODE_SUCCESS) {
    fprintf(stderr, "[%d] Failed to initialize UCVM\n", myid);
    return(1);
  }

  /* Add models */
  if (ucvm_add_model_list(cfg.ucvmstr) != UCVM_CODE_SUCCESS) {
    fprintf(stderr, "[%d] Failed to enable model list %s\n", myid,
	    cfg.ucvmstr);
    return(1);
  }

  /* Set depth query mode */
  if (ucvm_setparam(UCVM_PARAM_QUERY_MODE, UCVM_COORD_GEO_DEPTH) 
      != UCVM_CODE_SUCCESS) {
    fprintf(stderr, "[%d] Set query mode failed\n", myid);
    return(1);
  }

  /* Set interpolation z range */
  if (ucvm_setparam(UCVM_PARAM_IFUNC_ZRANGE, 
		    cfg.ucvm_zrange[0], 
		    cfg.ucvm_zrange[1]) != UCVM_CODE_SUCCESS) {
    fprintf(stderr, "[%d] Failed to set interpolation z range\n", myid);
    return(1);
  }

  ntask=cfg.proc_dims.dim[0]*cfg.proc_dims.dim[1]*cfg.proc_dims.dim[2];
  mytask=myid;

//  fprintf(stdout,"[%d] total tasks expected is %d\n",myid, ntask);
  stat_t stats[STAT_MAX_STATS]; // per process
  stat_t task_stats[STAT_MAX_STATS]; // per process

  memset(stats, 0, STAT_MAX_STATS*sizeof(stat_t));
  stats[STAT_MIN_VP].val = 100000.0;
  stats[STAT_MIN_VS].val = 100000.0;
  stats[STAT_MIN_RHO].val = 100000.0;
  stats[STAT_MIN_RATIO].val = 100000.0;

  while (mytask < ntask) {
        fprintf(stdout,"[%d:%d] XX looping before extract.. %d\n",myid, mytask, ntask);
        fflush(stdout);
  	/* Perform extractions */
  	if (extract(myid, mytask, ntask, &cfg, &task_stats[0]) != 0) {
                fprintf(stderr,"[%d:%d] failed to extract..",myid, mytask);
         	return(1);
  	}
        fprintf(stdout,"[%d:%d] XX looping after extract.. %d\n",myid, mytask, ntask);
        fflush(stdout);
        
   
  	if (task_stats[STAT_MAX_VP].val > stats[STAT_MAX_VP].val) {
  		memcpy(&stats[STAT_MAX_VP], &task_stats[STAT_MAX_VP], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MAX_VS].val > stats[STAT_MAX_VS].val) {
  		memcpy(&stats[STAT_MAX_VS], &task_stats[STAT_MAX_VS], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MAX_RHO].val > stats[STAT_MAX_RHO].val) {
  		memcpy(&stats[STAT_MAX_RHO], &task_stats[STAT_MAX_RHO], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MIN_VP].val < stats[STAT_MIN_VP].val) {
  		memcpy(&stats[STAT_MIN_VP], &task_stats[STAT_MIN_VP], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MIN_VS].val < stats[STAT_MIN_VS].val) {
  		memcpy(&stats[STAT_MIN_VS], &task_stats[STAT_MIN_VS], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MIN_RHO].val < stats[STAT_MIN_RHO].val) {
  		memcpy(&stats[STAT_MIN_RHO], &task_stats[STAT_MIN_RHO], sizeof(stat_t));
  	}
  	if (task_stats[STAT_MIN_RATIO].val < stats[STAT_MIN_RATIO].val) {
  		memcpy(&stats[STAT_MIN_RATIO], &task_stats[STAT_MIN_RATIO], sizeof(stat_t));
  	}
        mytask=mytask+nproc;
 }
   fprintf(stdout,"[%d:%d] XX out of looping from %d\n",myid, mytask,ntask);
   fflush(stdout);
	
  mpi_barrier();

  /* MPI Statistic vars */
  MPI_Datatype MPI_STAT_T;
  int num_fields_stat;
  stat_t *rbuf;

  /* Register new mesh data types */
  mpi_register_stat_4(&MPI_STAT_T, &num_fields_stat);

  /* Allocate statistics buffer */
  fprintf(stdout,"[%d] doing statistics...\n",myid);
  fflush(stdout);
  rbuf = (stat_t *)malloc(nproc*STAT_MAX_STATS*sizeof(stat_t));
  /* Gather stats */

  MPI_Gather( &stats[0], STAT_MAX_STATS, MPI_STAT_T, rbuf, STAT_MAX_STATS,
                    MPI_STAT_T, 0, MPI_COMM_WORLD);

  if (myid == 0) { 
    for (i = 0; i < nproc*STAT_MAX_STATS; i++) {
      switch (i % STAT_MAX_STATS) {
      case STAT_MAX_VP:
	if (rbuf[i].val > stats[STAT_MAX_VP].val) {
	  memcpy(&stats[STAT_MAX_VP], &rbuf[i], sizeof(stat_t));
	}
	break;
      case STAT_MAX_VS:
	if (rbuf[i].val > stats[STAT_MAX_VS].val) {
	  memcpy(&stats[STAT_MAX_VS], &rbuf[i], sizeof(stat_t));
	}
	break;
      case STAT_MAX_RHO:
	if (rbuf[i].val > stats[STAT_MAX_RHO].val) {
	  memcpy(&stats[STAT_MAX_RHO], &rbuf[i], sizeof(stat_t));
	}
	break;
      case STAT_MIN_VP:
	if (rbuf[i].val < stats[STAT_MIN_VP].val) {
	  memcpy(&stats[STAT_MIN_VP], &rbuf[i], sizeof(stat_t));
	}
	break;
      case STAT_MIN_VS:
	if (rbuf[i].val < stats[STAT_MIN_VS].val) {
	  memcpy(&stats[STAT_MIN_VS], &rbuf[i], sizeof(stat_t));
	}
	break;
      case STAT_MIN_RHO:
	if (rbuf[i].val < stats[STAT_MIN_RHO].val) {
	  memcpy(&stats[STAT_MIN_RHO], &rbuf[i], sizeof(stat_t));
	}
      case STAT_MIN_RATIO:
	if (rbuf[i].val < stats[STAT_MIN_RATIO].val) {
	  memcpy(&stats[STAT_MIN_RATIO], &rbuf[i], sizeof(stat_t));
	}
	break;
      default:
	fprintf(stderr, "[%d] Unexpected stat type %d", myid,
		i % STAT_MAX_STATS);
	return(1);
      }
    }
    for (j = 0; j < STAT_MAX_STATS; j++) {
      printf("[%d] %s: %f at\n", myid, stat_get_label(j), stats[j].val);
      printf("[%d]\ti,j,k : %d, %d, %d\n", myid, 
	     stats[j].i, stats[j].j, stats[j].k);
      fflush(stdout);
    }
  }
  /* Free statistics buffer */
  free(rbuf);

  /* Stage out mesh file(s) */
  if ((myid == 0) && (strlen(stageoutdir) > 0)) {
    fprintf(stdout,"[%d] Staging out mesh file(s)\n", myid);
    if (cfg.meshtype == MESH_FORMAT_SORD) {
      sprintf(tmp, "%s_*", cfg.meshfile);
      sprintf(tmp2, "%s", stageoutdir);
      printf("[%d] Copying %s to %s\n", myid, tmp, tmp2);
      if (copyFile(tmp, tmp2) != 0) {
	fprintf(stderr, 
		"[%d] Failed to copy vp/vs/rho mesh to stage out dir\n", 
		myid);
	return(1);
      }
    } else {
      fprintf(stdout,"[%d] Copying %s to %s\n", myid, cfg.meshfile, stageoutdir);
      if (copyFile(cfg.meshfile, stageoutdir) != 0) {
	fprintf(stderr, "[%d] Failed to copy mesh to stage out dir\n", 
		myid);
	return(1);
      }
    }
    fprintf(stdout,"[%d] Copying %s to %s\n", myid, cfg.gridfile, stageoutdir);
    if (copyFile(cfg.gridfile, stageoutdir) != 0) {
      fprintf(stderr, "[%d] Failed to copy mesh to stage out dir\n", 
	      myid);
      return(1);
    }
  }

  /* Final sync */
  mpi_barrier();
  mpi_final("MPI Done");

  return(0);
}
