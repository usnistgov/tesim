/*

* Author:       Rick Candell (rick.candell@nist.gov)
*               Tim Zimmerman (timothy.zimmerman@nist.gov)
* Organization: National Institute of Standards and Technology
*               U.S. Department of Commerce
* License:      Public Domain

*/
// tesim.cpp : Defines the main() entry point for the console application.
//

//#include <boost/system/config.hpp>
#include <boost/timer/timer.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include "TENames.h"
#include "TEIIDErrorChannel.h"
#include "TEGEErrorChannel.h"
#include "TETimeSync.h"
#include "TEPlant.h"
#include "TEController.h"
#ifdef USE_ADS_IF
#include "TEADSInterface.h"
#endif
#include <fstream>
#include <iostream>
#include <iomanip>
#include <math.h> 
#include <utility>

#define XMV_SHMEM_NAME ("xmv_shmem")
#define IDV_SHMEM_NAME ("idv_shmem")
#define SIM_SHMEM_NAME ("sim_shmem")
#define SP_SHMEM_NAME  ("sp_shmem")

// function prototypes
void print_sim_params(double tstep, double tscan, double simtime, bool rt, pq_pair xmeas_pq, pq_pair xmv_pq);

using namespace boost::interprocess;

typedef struct { int index; double value; } xmv_pair;
typedef struct { int index; int value;    } idv_pair;

struct shm_remove
{
	shm_remove()
	{
		shared_memory_object::remove(SIM_SHMEM_NAME);
		shared_memory_object::remove(XMV_SHMEM_NAME);
		shared_memory_object::remove(IDV_SHMEM_NAME);
		shared_memory_object::remove(SP_SHMEM_NAME);
	}
	~shm_remove()
	{
		shared_memory_object::remove(SIM_SHMEM_NAME);
		shared_memory_object::remove(XMV_SHMEM_NAME);
		shared_memory_object::remove(IDV_SHMEM_NAME);
		shared_memory_object::remove(SP_SHMEM_NAME); 
	}
};

int main(int argc, char* argv[])
{
	/* 
	Declaration of variables for main line program
	*/
	std::string log_file_prefix = "nochan";
	bool append_flag = false, RT = false, use_ads = false, ads_remote = false, 
		gechan_on = false, enable_idv = false, shdmem_on = false, ext_control = false;
	double simtime = 0.0, t = 0.0, tstep = 0.0005, tscan = 0.0005;
	unsigned ksave = 20, idv_idx = 0;
	double *xmeas, *xmv;

	/*
	Set point values.  Default values for setpoints are assigned here.
	tesim allows for override of these setpoints at time t=0 throgh the command line program options.
	*/
	double prod_rate_sp = 22.89, reactor_pressure_sp = 2800.0, reactor_level_sp = 65.0, reactor_temp_sp = 122.9,
		pctg_sp = 53.8, sep_level_sp = 50.0, stripper_level_sp = 50.0;

	// print the simulation parameters
	std::cout << "TE Simulator C++" << std::endl
		<< "  developed by Rick Candell and Tim Zimmerman" << std::endl
		<< "  National Institure of Standards and Technology (USA)" << std::endl
		<< "  Software is Public Domain" << std::endl
		<< "******************************************************" << std::endl << std::endl;

	// error channel parameters
	double per = 0.0;
	pq_pair xmeas_pq = std::make_pair(0.0, 1.0);
	pq_pair xmv_pq = std::make_pair(0.0, 1.0);

	// program options
	namespace po = boost::program_options;
	po::options_description desc("Allowed options");
	desc.add_options()
		("help,h", "print the help message")
		("simtime,s", po::value<double>(&simtime)->required(), "set the simulation time in hours")
		("tstep,t", po::value<double>(&tstep), "set the base time step in hours")
		("tscan,c", po::value<double>(&tscan), "set the scan interval in hours")
		("ksave,k", po::value<unsigned>(&ksave), "decimation factor for saving trace data")
		("real-time,r", po::bool_switch(&RT)->default_value(false), "run the simulation in real time")
		("logfile-prefix,p", po::value<std::string>(&log_file_prefix), "prefix for all of the log files")
		("append-data,a", po::bool_switch(&append_flag)->default_value(false), "append plant data to output file")

		// disturbances
		("setidv,i", po::value<unsigned>(&idv_idx), "set the idv at index provided (1 - 20)")

		// shared memory
		("shared-memory", po::bool_switch(&shdmem_on)->default_value(false), "xmv and idv variables")
		("external-ctrl", po::bool_switch(&ext_control)->default_value(false), "read xmv from shared memory")

#ifdef USE_ADS_IF
		// ADS interface
		("enable-ads", po::bool_switch(&use_ads)->default_value(false), "turns on the ADS interface to PLC")
		("ads-remote", po::bool_switch(&ads_remote)->default_value(false), "enables remote connection to 5.20.215.224.1.1")
#endif

		// packet error rate parameters
		("per", po::value<double>(&per), "Enable iid packet error rate between 0.0 and 1.0.")
		("enable-ge-channel,g", po::bool_switch(&gechan_on)->default_value(false), "Enable the Gilbert Elliot channel model.")
		("xmeas-pq", po::value<pq_pair>(&xmeas_pq), "xmeas GE probs as (Perror:Precover)")
		("xmv-pq", po::value<pq_pair>(&xmv_pq), "xmv GE probs as (Perror:Precover)")

		// set point overrides
		("sp-prod-rate", po::value<double>(&prod_rate_sp),					"change setpoint at Tstart (default: 22.89)")
		("sp-reactor-pressure", po::value<double>(&reactor_pressure_sp),	"change setpoint at Tstart (default: 2800.)")
		("sp-reactor-level", po::value<double>(&reactor_level_sp),			"change setpoint at Tstart (default: 65.0 )")
		("sp-reactor-temp", po::value<double>(&reactor_temp_sp),			"change setpoint at Tstart (default: 122.9)")
		("sp-pctg", po::value<double>(&pctg_sp),							"change setpoint at Tstart (default: 53.8 )")
		("sp-separator-level", po::value<double>(&sep_level_sp),			"change setpoint at Tstart (default: 50.0 )")
		("sp-stripper-level", po::value<double>(&stripper_level_sp),		"change setpoint at Tstart (default: 50.0 )")		
		;

	po::variables_map vm;
	try	{
		po::store(po::parse_command_line(argc, argv, desc), vm);
		if (vm.count("help"))
		{
			std::cout << desc << std::endl;
			return 0;
		}

		po::notify(vm);
	}
	catch (po::error& e) {
		std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
		std::cerr << desc << std::endl;
		return 0;
	}
	po::notify(vm);

	try 
	{
		if (vm.count("setidv"))		{ enable_idv = true; }
	}
	catch (po::error& e) {
		std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
		std::cerr << desc << std::endl;
		return 0;
	}

	// disable shared memory interface in fast mode
	if (!RT && shdmem_on)
	{
		std::cout << "shared memory mode requires real-time mode" << std::endl;
		return(1);
	}
	if ( !(shdmem_on && RT) && ext_control )
	{
		std::cout << "enabling real-time mode and shared memory for external control" << std::endl;
		shdmem_on = true;
		RT = true;
	}
	if (!RT) shdmem_on = false;

	std::cout << "Simulation time : " << simtime << std::endl;
	std::cout << "Tstep:                       " << tstep << std::endl;
	std::cout << "Tscan:                       " << tscan << std::endl;
	std::cout << "Ksave:                       " << ksave << std::endl;
	std::cout << "log file prefix:             " << log_file_prefix << std::endl;
	std::cout << "Append:                      " << append_flag << std::endl;
	std::cout << "Run RT:                      " << RT << std::endl;
	std::cout << "Use ADS:                     " << use_ads << std::endl;
	std::cout << "Use remote ADS:              " << ads_remote << std::endl;
	std::cout << "IID chan per:                " << per << std::endl;
	std::cout << "Enabled GE chan:             " << gechan_on << std::endl;
	std::cout << "GE chan xmeas:               " << xmeas_pq << std::endl;
	std::cout << "GE chan xmv:                 " << xmv_pq << std::endl;
	std::cout << "Enable IDV:                  " << enable_idv << std::endl;
	std::cout << "IDV index:                   " << idv_idx << std::endl;
	std::cout << "Enable shdmem:               " << shdmem_on << std::endl;
	std::cout << "Use ext. controller:         " << ext_control << std::endl;
	std::cout << "Prod Rate:                   " << prod_rate_sp << (vm.count("sp-prod-rate") ? " override" : "  default") << std::endl;
	std::cout << "Reactor pressure:            " << reactor_pressure_sp << (vm.count("sp-reactor-pressure") ? " override" : "  default") << std::endl;
	std::cout << "Reactor level:               " << reactor_level_sp << (vm.count("sp-reactor-level") ? " override" : "  default") << std::endl;
	std::cout << "Reactor temp:                " << reactor_temp_sp << (vm.count("sp-reactor-temp") ? " override" : "  default") << std::endl;
	std::cout << "Pct G:                       " << pctg_sp << (vm.count("sp-pctg") ? " override" : "  default") << std::endl;
	std::cout << "Sep level:                   " << sep_level_sp << (vm.count("sp-separator-level") ? " override" : "  default") << std::endl;
	std::cout << "Strip level:                 " << stripper_level_sp << (vm.count("sp-stripper-level") ? " override" : "  default") << std::endl;
	std::cout << std::endl;

	TEPlant* teplant = TEPlant::getInstance();
	TEController* tectlr = TEController::getInstance();

	// Create the log files
	std::ofstream plant_log;
	std::ofstream ctlr_log;
	std::ofstream time_log;
	std::ofstream xmeas_chan_log;
	std::ofstream xmv_chan_log;

	// create shared memory for control of the plant
	using namespace boost::interprocess;
	shared_memory_object * xmv_shm = 0, *idv_shm = 0, *sim_shm = 0, *sp_shm = 0;
	shm_remove remover;
	if (shdmem_on && RT)
	{
		try
		{
			idv_shm = new shared_memory_object(open_or_create, IDV_SHMEM_NAME, read_write);
			idv_shm->truncate(sizeof(idv_pair));
			std::cout << "idv shared memory object created" << std::endl;

			xmv_shm = new shared_memory_object(open_or_create, XMV_SHMEM_NAME, read_write);
			xmv_shm->truncate(sizeof(double)*TEPlant::NU);
			std::cout << "xmv shared memory object created" << std::endl;

			sim_shm = new shared_memory_object(open_or_create, SIM_SHMEM_NAME, read_write);
			sim_shm->truncate(sizeof(double)*TEPlant::NY + sizeof(double)*TEPlant::NU);
			std::cout << "xmeas shared memory object created" << std::endl;

			sp_shm = new shared_memory_object(open_or_create, SP_SHMEM_NAME, read_write);
			sp_shm->truncate(sizeof(sp_pair));
			std::cout << "setpoints shared memory object created" << std::endl;
		}
		catch (interprocess_exception & ex)
		{
			std::cout << "shared memory error: " << ex.what() << std::endl;
			return 1;
		}
	}

	// do we append or create a new log file?
	if (append_flag)
	{
		plant_log.open(log_file_prefix + "_plant.dat", std::fstream::out | std::fstream::app);
		std::cout << "plant data file: " << log_file_prefix << "_plant.dat" << std::endl;
	}
	else
	{
		plant_log.open(log_file_prefix + "_plant.dat");
		plant_log << TENames::plant_all() << std::endl;
	}
	plant_log.precision(15);

	ctlr_log.open(log_file_prefix + "_tectlr.dat");
	ctlr_log.precision(15);

	// if we run in real-time, then create a time synch log
	if (RT)
	{
		time_log.open(log_file_prefix + "_time.dat");
		time_log.precision(15);
		time_log.fill('0');
	}

	if (gechan_on || per>0.0)
	{
		xmeas_chan_log.open(log_file_prefix + "_xmeas_chan.log");
		xmv_chan_log.open(log_file_prefix + "_xmv_chan.log");
	}

#ifdef USE_ADS_IF
	// setup the ads interface
	TEADSInterface ads, ads_mbs;
	if (use_ads)
	{
		if (ads_remote)
		{
			AmsAddr plc_addr;
			plc_addr.netId.b[0] = 5;
			plc_addr.netId.b[1] = 20; 
			plc_addr.netId.b[2] = 215; 
			plc_addr.netId.b[3] = 224;
			plc_addr.netId.b[4] = 1; 
			plc_addr.netId.b[5] = 1;
			plc_addr.port = 851;
			ads.connect("G_IO.XMEAS", &plc_addr);
			ads_mbs.connect("G_IO.MBS_XMEAS", &plc_addr);
		}
		else
		{
			ads.connect("MAIN.XMEAS", 851);
			ads_mbs.connect("G_IO.MBS_XMEAS", 851);
		}
	}
#endif 

	// plant shutdown indicator
	int shutdown = 0;
	char * plant_msg = NULL;

	// derived simulation parameters
	int nsteps = int(simtime/tstep) + 1;
	int steps_per_scan = (int)round(tscan / tstep);
	//print_sim_params(tstep, tscan, simtime, RT, xmeas_pq, xmv_pq);

	// auto timer used as a performance profiler
	boost::timer::auto_cpu_timer t_wall_auto;

	// being time synch with wall clock
	TETimeSync tesync;
	tesync.init();

	// init the controller
	tectlr->initialize(tscan);
	xmv = (double*)(tectlr->get_xmv());

	// apply the overrides to the set-points
	// this must be performed after the initialize call for obvious reasons
	if (vm.count("sp-prod-rate"))			{ tectlr->prod_rate_sp(prod_rate_sp); }
	if (vm.count("sp-reactor-pressure"))	{ tectlr->reactor_pressure_sp(reactor_pressure_sp); }
	if (vm.count("sp-reactor-level"))		{ tectlr->reactor_level_sp(reactor_level_sp); }
	if (vm.count("sp-reactor-temp"))		{ tectlr->reactor_temp_sp(reactor_temp_sp); }
	if (vm.count("sp-pctg"))				{ tectlr->pctg_sp(pctg_sp); }
	if (vm.count("sp-separator-level"))		{ tectlr->sep_level_sp(sep_level_sp); }
	if (vm.count("sp-stripper-level"))		{ tectlr->strip_level_sp(stripper_level_sp); }

	// init the plant
	teplant->initialize();
	xmeas = (double*)(teplant->get_xmeas());

	// create the communications channels
	int seed_rand = 17;
	TEErrorChannel* xmeas_channel = 0;
	TEErrorChannel* xmv_channel = 0;
	if (per > 0.0)
	{
		xmeas_channel = new TEIIDErrorChannel(per, TEPlant::NY, teplant->get_xmeas(), seed_rand);
		xmv_channel = new TEIIDErrorChannel(per, TEPlant::NU, tectlr->get_xmv(), seed_rand);
	}
	else if (gechan_on)
	{
		xmeas_channel = new TEGEErrorChannel(xmeas_pq, TEPlant::NY, teplant->get_xmeas(), seed_rand);
		xmv_channel = new TEGEErrorChannel(xmv_pq, TEPlant::NU, tectlr->get_xmv(), seed_rand);
	}
	
	for (int ii = 0; ii < nsteps; ii++)
	{
		// increment the plant and controller
		try
		{
			// examine shared memory for disturbances
			if (shdmem_on && RT)
			{
				// first update the disturbance vector
				mapped_region reg_idv(*idv_shm, read_write);
				idv_pair *mem = static_cast<idv_pair*>(reg_idv.get_address());
				teplant->idv(mem->index, mem->value);
			}

			// set the disturbance to hold on from command line
			if (enable_idv)
			{
				teplant->idv(idv_idx-1);
			}

			// increment the plant
			xmeas = teplant->increment(t, tstep, xmv, &shutdown);

			if (shdmem_on && RT)
			{
				// copy the xmeas values to shared memory
				mapped_region reg_sim(*sim_shm, read_write);
				double *shm_sim = static_cast<double*>(reg_sim.get_address());
				memcpy(shm_sim, xmeas, sizeof(double)*TEPlant::NY);
				memcpy(shm_sim + TEPlant::NY, xmv, sizeof(double)*TEPlant::NU);
			}

			// send the measured variables to the PLC
#ifdef USE_ADS_IF
			if (use_ads) { ads.write(xmeas); }
#endif

			// apply the sensors channel
			if (per > 0.0)
			{
				xmeas = *static_cast<TEIIDErrorChannel*>(xmeas_channel)+xmeas;
				xmeas_chan_log << *static_cast<TEIIDErrorChannel*>(xmeas_channel) << std::endl;
			}
			else if (gechan_on)
			{
				xmeas = *static_cast<TEGEErrorChannel*>(xmeas_channel) + xmeas;
				xmeas_chan_log << *static_cast<TEGEErrorChannel*>(xmeas_channel) << std::endl;
			}
		}
		catch (TEPlant::ShutdownException& e)
		{
			plant_log << xmeas_pq << "\t" << xmv_pq << "\t" << t << "\t" << *teplant 
				<< "\t" << e.m_sd_code << std::endl;
			std::cerr << e << std::endl;
			std::cerr << "ending simulation" << std::endl;
			return 0;
		}

		// run the controller if time is at a scan boundary
		if (!(ii%steps_per_scan))
		{
			// query the modbus server
#ifdef USE_ADS_IF
			if (use_ads)
			{
				float mbs_xmeas[2];
				ads_mbs.read(mbs_xmeas);
				xmeas[6] = mbs_xmeas[0];
				xmeas[7] = mbs_xmeas[1];
				//std::cout << "mbs xmeas: " << xmeas[6] << " " << xmeas[7] << std::endl;
			}
#endif

			// increment the controller
			if (! (ext_control  && shdmem_on && RT) )
			{
				xmv = tectlr->increment(t, tscan, xmeas);
			}
			else
			{
				// apply new manipulation updates from external controller
				mapped_region reg_xmv(*xmv_shm, read_write);
				double * mem = static_cast<double*>(reg_xmv.get_address());
				memcpy(xmv, mem, sizeof(double)*TEPlant::NU);
			}

			// apply the control channel
			if (per > 0.0)
			{
				xmv = *static_cast<TEIIDErrorChannel*>(xmv_channel)+xmv;
				xmv_chan_log << *static_cast<TEIIDErrorChannel*>(xmv_channel) << std::endl;
			}
			else if (gechan_on)
			{
				xmv = *static_cast<TEGEErrorChannel*>(xmv_channel) + xmv;
				xmv_chan_log << *static_cast<TEGEErrorChannel*>(xmv_channel) << std::endl;
			}

			// check for any setpoint overrides to apply
			if (shdmem_on && RT)
			{
				// apply new manipulation updates from external controller
				mapped_region reg_sp(*sp_shm, read_write);
				sp_override_pair * mem = static_cast<sp_override_pair*>(reg_sp.get_address());
				if (mem->first)
				{
					switch (mem->second.first)
					{
					case TEController::PROD_RATE:
						std::cout << "changing production rate to " << mem->second.second << std::endl;
						tectlr->prod_rate_sp(mem->second.second);
						break;
					case TEController::REACTOR_PRESS:
						std::cout << "changing reactor pressure to " << mem->second.second << std::endl;
						tectlr->reactor_pressure_sp(mem->second.second);
						break;
					case TEController::REACTOR_LEVEL:
						std::cout << "changing reactor level to " << mem->second.second << std::endl;
						tectlr->reactor_level_sp(mem->second.second);
						break;
					case TEController::REACTOR_TEMP:
						std::cout << "changing reactor temp to " << mem->second.second << std::endl;
						tectlr->reactor_temp_sp(mem->second.second);
						break;
					case TEController::SEP_LEVEL:
						std::cout << "changing separator level to " << mem->second.second << std::endl;
						tectlr->sep_level_sp(mem->second.second);
						break;
					case TEController::STRIP_LEVEL:
						std::cout << "changing stripper level to " << mem->second.second << std::endl;
						tectlr->strip_level_sp(mem->second.second);
						break;
					case TEController::PCTG:
						std::cout << "changing pctg to " << mem->second.second << std::endl;
						tectlr->pctg_sp(mem->second.second);
						break;
					default:
						std::cout << "unknown setpoint specifiy" << std::endl;
						break;
					}
					mem->first = false;
				}
			}
		}

		// log plant and controller data
		if (!(ii%ksave))
		{
			//plant
			plant_log << xmeas_pq << "\t" << xmv_pq << "\t" << t << "\t" << *teplant << "\t" << 0 << std::endl;

			//controller
			ctlr_log << t << "\t" << *tectlr << std::endl;
		}

		// try to sync sim time to match wall clock time
		if (RT)
		{
			dbl_sec sim_time_dur(t * 3600.0);
			tesync.sync(sim_time_dur, time_log);
		}

		// log current time to console
		std::cout << "\r" << "time: " << std::setprecision(8) << std::setfill('0') << t << " hours            ";

		// Increment to the next time step
		// Approximation of tstep because of limited memory causes errors to 
		// integrate over time (round-off error), so we must recalculate t on 
		// every iteration using a method that truncates the floating point error.
		t = (double)(ii + 1) * tstep;
	}

	// destroy the shared memory objects
	if (xmv_shm) delete xmv_shm;
	if (sim_shm) delete sim_shm;
	if(idv_shm) delete idv_shm;
	if (sp_shm) delete sp_shm;

	std::cout << std::endl;
	return 0;
}

void print_sim_params(double tstep, double tscan, double simtime, bool rt, pq_pair xmeas_pq, pq_pair xmv_pq)
{
	BOOST_LOG_TRIVIAL(info) << std::endl << "TE simulation" << std::endl
		<< "simulation time: " << simtime << " hrs" << std::endl
		<< "plant dt: " << tstep << " hrs" << std::endl
		<< "ctlr dt: " << tscan << " hrs" << std::endl
		<< "Real-time: " << rt << std::endl
		<< "PER (xmeas): " << xmeas_pq << std::endl
		<< "PER (xmv): " << xmv_pq << std::endl;
}

