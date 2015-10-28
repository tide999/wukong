#pragma once 

#include <map>
#include <string>
#include <fstream>
#include <iostream>
using namespace std;
extern bool global_use_rbf;
extern bool global_use_rdma;
extern int global_rdma_threshold;
extern int global_query_type;
extern int global_num_server;
extern int global_num_client;
extern int global_batch_factor;
extern string global_input_folder;
extern bool global_interactive;
extern int global_rdftype_id;
void load_global_cfg(char* filename){
	ifstream file(filename);
	
	string row;
	string val;
	if(!file)
		cout<<"File "<<filename<<" not exist"<<endl;
	map<string,string> config_map;
	while(file>>row>>val){
		config_map[row]=val;
	}
	global_use_rbf=atoi(config_map["global_use_rbf"].c_str());
	global_use_rdma=atoi(config_map["global_use_rdma"].c_str());
	global_rdma_threshold=atoi(config_map["global_rdma_threshold"].c_str());
	global_query_type=atoi(config_map["global_query_type"].c_str());
	global_num_server=atoi(config_map["global_num_server"].c_str());
	global_num_client=atoi(config_map["global_num_client"].c_str());
	global_batch_factor=atoi(config_map["global_batch_factor"].c_str());
	global_input_folder=config_map["global_input_folder"];
	global_interactive=atoi(config_map["global_interactive"].c_str());
	global_rdftype_id=atoi(config_map["global_rdftype_id"].c_str());
}


