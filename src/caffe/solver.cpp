#include <cstdio>

#include <algorithm>
#include <string>
#include <vector>
#include <queue>
#include <sys/time.h>

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>

#include "caffe/solver.hpp"
#include "caffe/sgd_solvers.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/upgrade_proto.hpp"
#include "caffe/util/mpi.hpp"
#include "leveldb/db.h"
#include "lmdb.h"
#include "caffe/data_layers.hpp"

using namespace std;

int currentUpdateCount=0;
atomic_int undoneIter{0};
mutex mutexUpdateWeight;
condition_variable condUpdateWeight;
condition_variable condUpdateNet;
mutex mutexUpdateNet;

namespace caffe {

Caffe::Brew mode=Caffe::mode();

#ifdef ASYNCTRAN
	template <typename Dtype>
		void SlaveReadData(shared_ptr<Net<Dtype> > net, const int endNum ,semaphore* semRead,semaphore* semNext){
			vector<Blob<Dtype>*>* top = net->getTopPointer();
			MPI_Status status;
			for(int i=0;i<endNum;++i){
				status.MPI_ERROR=0;
#ifdef DIRECTGPU
				caffe_mpi_recv<Dtype>((*top)[0]->mutable_gpu_data(),(*top)[0]->count(),
						0,TAG_DATA_OUT,MPI_COMM_WORLD,&status);
				DLOG(INFO)<<"Recv Dataout status "<<status.MPI_ERROR;
				if (top->size()>1) {
					caffe_mpi_recv<Dtype>((*top)[1]->mutable_gpu_data(),(*top)[1]->count(),
							0,TAG_DATA_OUT_IF,MPI_COMM_WORLD,&status);
					DLOG(INFO)<<"Recv Dataout status "<<status.MPI_ERROR;
				}
#else
				caffe_mpi_recv<Dtype>((*top)[0]->mutable_cpu_data(),(*top)[0]->count(),
						0,TAG_DATA_OUT,MPI_COMM_WORLD,&status);
				DLOG(INFO)<<"Recv Dataout status "<<status.MPI_ERROR;
				if (top->size()>1) {
					caffe_mpi_recv<Dtype>((*top)[1]->mutable_cpu_data(),(*top)[1]->count(),
							0,TAG_DATA_OUT_IF,MPI_COMM_WORLD,&status);
					DLOG(INFO)<<"Recv Dataout status "<<status.MPI_ERROR;
				}
#endif
				semRead->notify();
				semNext->wait();
			}
			LOG(INFO)<<"Slave read thread finished!";
		}
#endif



template <typename Dtype> 
                void ReadDataFromDBandDistribute(Layer<Dtype>* layer, const int tid, const int endNum, const int childProcessSum){
			int startNum = 0;
			int batchsize = layer->layer_param().data_param().batch_size();
			int skip; 
		        if(tid<5) 
			  	skip = (tid-1)*batchsize;
			else if(tid<10)
			  	skip = (tid-2)*batchsize;
			else if(tid<15)
			  	skip = (tid-3)*batchsize;
			else 
			  	skip = (tid-4)*batchsize;
			Blob<Dtype> *prefetch_data_ = new Blob<Dtype>;
			Blob<Dtype> *prefetch_label_ = new Blob<Dtype>;
			
			/*****leveldb *******/
			shared_ptr<leveldb::Iterator> titer_;
			/*****LMDB **********/
			MDB_cursor* mdb_cursor_;
			MDB_val mdb_key_, mdb_value_;
			
			shared_ptr<db::DB> db(db::GetDB(layer->layer_param().data_param().backend()));
  			db->Open(layer->layer_param().data_param().source(), db::READ);
  			shared_ptr<db::Cursor> cursor(db->NewCursor());
			cursor->SeekToFirst();	

			bool output_label = layer->getOutputLabel();

                        layer->reshapeData(*prefetch_data_, *prefetch_label_);
                        const int skip1 = (childProcessSum-1) * batchsize;
for(int Num = startNum; Num < endNum; ++Num){
                                if(Num!= startNum)skip = skip1;
                                DBGPRT(LOG(INFO)<<"SKIP START "<< Num <<" " <<tid<<" "<<skip);
#if 1
                                while (skip-- > 0) {
				cursor->Next();
					if(!(cursor->valid()))
						cursor->SeekToFirst();
						
                                }

#endif
                                DBGPRT(LOG(INFO)<<"SKIP FIN READ START"<<Num<<" "<<tid); //gz cursor
                                layer->ReadData(cursor, *prefetch_data_, *prefetch_label_);
                                DBGPRT(LOG(INFO)<<"READ FIN SEND START"<<Num<<" "<<tid);
#ifdef DIRECTGPU
                                caffe_mpi_send<Dtype>(prefetch_data_->mutable_gpu_data(),prefetch_data_->count(),
                                                tid,TAG_DATA_OUT,MPI_COMM_WORLD);
				if(output_label){
                                        caffe_mpi_send<Dtype>(prefetch_label_->mutable_gpu_data(),prefetch_label_->count(),
                                                        tid,TAG_DATA_OUT_IF,MPI_COMM_WORLD);
                                }
#else
                                caffe_mpi_send<Dtype>(prefetch_data_->mutable_cpu_data(),prefetch_data_->count(),
                                                tid,TAG_DATA_OUT,MPI_COMM_WORLD);
                                if(output_label){
                                        caffe_mpi_send<Dtype>(prefetch_label_->mutable_cpu_data(),prefetch_label_->count(),
                                                        tid,TAG_DATA_OUT_IF,MPI_COMM_WORLD);
                                }
#endif
                                DBGPRT(LOG(INFO)<<"SEND FIN"<<Num<<" "<<tid);
			}

			delete prefetch_data_;
			delete prefetch_label_;
			LOG(INFO)<<"Read database thread out! Thread No."<<tid;
		
		}
		
		
		
template <typename Dtype>
		void ComputeValueThreadServer( Solver<Dtype>* layer, const int childProcessSum) {
		bool waitStatus;
			while(true){
				if(undoneIter <= 0)break;
				{
					waitStatus=false;
					unique_lock<mutex> lk(mutexUpdateWeight);
#if 1 
					DBGPRT(LOG(INFO)<<"WAIT "<<currentUpdateCount);
					waitStatus=condUpdateWeight.wait_for(lk,chrono::seconds(layer->param().timeout_sec()),
							[=](){return currentUpdateCount >= childProcessSum;});
					
				
				if(waitStatus==false)
					LOG(INFO)<<"Timeout "<<currentUpdateCount;
#else
					condUpdateWeight.wait(lk,[=](){return currentUpdateCount >= childProcessSum || currentUpdateCount >= undoneIter;});//need test
#endif
					DBGPRT(LOG(INFO)<<"WAIT FIN"<<currentUpdateCount);
					undoneIter -= currentUpdateCount;
					if(currentUpdateCount>0){
						layer->ComputeValueServer();
						DBGPRT(LOG(INFO)<<"CVS FIN"<<currentUpdateCount);
						condUpdateNet.notify_all();

					}
				}
				
			}
		}
	template <typename Dtype>
		void Solver<Dtype>::ComputeValueServer(){
			ComputeUpdateValue();
			bool ifTest = ((iter_+currentUpdateCount)/param_.test_interval())  > (iter_ /param_.test_interval());
			iter_ += currentUpdateCount;
			if(ifTest)
				TestAll();
			currentUpdateCount=0;
		}
		
		template <typename Dtype>
		void ComputeValueThreadClient(Solver<Dtype>* solver, const int tid, const int endNum) {
			int startNum = 0;
			CHECK(solver);
			for(int i=startNum; i < endNum; ++i){
				solver->ComputeValueClient(tid);
			}
			LOG(INFO)<<"Thread fin "<<tid;
		}
	template <typename Dtype>
		void Solver<Dtype>::ComputeValueClient(int tid){
			int mpi_source,tid22;

if(tid<10)
{
	if(tid <5) 
		tid22 = tid;
	else
		tid22 = tid-1;
}
else 
{
	if(tid<15)
		tid22 =tid-2;
	else
		tid22 =tid-3;
}
			ComputeUpdateValueClientThread(mpi_source,tid,tid22);
			{
				unique_lock<mutex> ul(mutexUpdateNet);
				condUpdateNet.wait(ul, []{return currentUpdateCount == 0;});
			}

			flagComputeEndNeedUpdate[tid22-1]=0;
			vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
			for (int param_id = 0; param_id < net_params.size(); ++param_id) {
				gpu_sync();
#ifdef DIRECTGPU
				caffe_mpi_send<Dtype>(tempdata[tid22-1][param_id]->mutable_gpu_data(),tempdata[tid22-1][param_id]->count(),
						tid,TAG_NET_OUT,MPI_COMM_WORLD);
#else
				caffe_mpi_send<Dtype>(tempdata[tid22-1][param_id]->mutable_cpu_data(),tempdata[tid22-1][param_id]->count(),
						tid,TAG_NET_OUT,MPI_COMM_WORLD);
#endif
			}	
		}
		
		template <typename Dtype>
Dtype Solver<Dtype>::GetLearningRate() {
  Dtype rate;
  const string& lr_policy = this->param_.lr_policy();
  if (lr_policy == "fixed") {
    rate = this->param_.base_lr();
  } else if (lr_policy == "step") {
    this->current_step_ = this->iter_ / this->param_.stepsize();
    rate = this->param_.base_lr() *
        pow(this->param_.gamma(), this->current_step_);
  } else if (lr_policy == "exp") {
    rate = this->param_.base_lr() * pow(this->param_.gamma(), this->iter_);
  } else if (lr_policy == "inv") {
    rate = this->param_.base_lr() *
        pow(Dtype(1) + this->param_.gamma() * this->iter_,
            - this->param_.power());
  } else if (lr_policy == "multistep") {
    if (this->current_step_ < this->param_.stepvalue_size() &&
          this->iter_ >= this->param_.stepvalue(this->current_step_)) {
      this->current_step_++;
      LOG(INFO) << "MultiStep Status: Iteration " <<
      this->iter_ << ", step = " << this->current_step_;
    }
    rate = this->param_.base_lr() *
        pow(this->param_.gamma(), this->current_step_);
  } else if (lr_policy == "poly") {
    rate = this->param_.base_lr() * pow(Dtype(1.) -
        (Dtype(this->iter_) / Dtype(this->param_.max_iter())),
        this->param_.power());
  } else if (lr_policy == "sigmoid") {
    rate = this->param_.base_lr() * (Dtype(1.) /
        (Dtype(1.) + exp(-this->param_.gamma() * (Dtype(this->iter_) -
          Dtype(this->param_.stepsize())))));
  } else {
    LOG(FATAL) << "Unknown learning rate policy: " << lr_policy;
  }
  return rate;
}
	template <typename Dtype>
		void Solver<Dtype>::ComputeUpdateValue() {
			vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
			const vector<float>& net_params_lr = this->net_->params_lr();
			const vector<float>& net_params_weight_decay = this->net_->params_weight_decay();
			Dtype rate = GetLearningRate();
			Dtype momentum = this->param_.momentum();
			Dtype weight_decay = this->param_.weight_decay();
			string regularization_type = this->param_.regularization_type();
			Caffe::set_mode(Caffe::GPU); 
	       		
		switch (Caffe::mode()) {
				case Caffe::CPU:
					for(int param_id = 0; param_id < net_params.size(); ++param_id){
						memset(net_params[param_id]->mutable_cpu_diff(),0,sizeof(Dtype)*(net_params[param_id]->count()));
					}
					for(int i=0;i<this->childProcessSum;++i){
						if(this->flagComputeEndNeedUpdate[i]==1){
							for(int param_id = 0; param_id < net_params.size(); ++param_id){
								caffe_axpy(net_params[param_id]->count(),(Dtype)1,
										this->tempdata[i][param_id]->mutable_cpu_data(),
										net_params[param_id]->mutable_cpu_diff());
							}
						}
					}
					for (int param_id = 0; param_id < net_params.size(); ++param_id) {
						caffe_scal(net_params[param_id]->count(),(Dtype)(1.0/currentUpdateCount),net_params[param_id]->mutable_cpu_diff());
						Dtype local_rate = rate * net_params_lr[param_id];
						Dtype local_decay = weight_decay * net_params_weight_decay[param_id];

						if (local_decay) {
							if (regularization_type == "L2") {
								caffe_axpy(net_params[param_id]->count(),
										local_decay,
										net_params[param_id]->cpu_data(),
										net_params[param_id]->mutable_cpu_diff());
							} else if (regularization_type == "L1") {
								caffe_cpu_sign(net_params[param_id]->count(),
										net_params[param_id]->cpu_data(),
										temp_[param_id]->mutable_cpu_data());
								caffe_axpy(net_params[param_id]->count(),
										local_decay,
										temp_[param_id]->cpu_data(),
										net_params[param_id]->mutable_cpu_diff());
							} else {
								LOG(FATAL) << "Unknown regularization type: " << regularization_type;
							}
						}

						caffe_cpu_axpby(net_params[param_id]->count(), local_rate,
								net_params[param_id]->cpu_diff(), momentum,
								history_[param_id]->mutable_cpu_data());
						caffe_copy(net_params[param_id]->count(),
								history_[param_id]->cpu_data(),
								net_params[param_id]->mutable_cpu_diff());
					}
					break;
				case Caffe::GPU:
#ifndef CPU_ONLY
	          				for(int param_id = 0; param_id < net_params.size(); ++param_id){
						cudaMemset(net_params[param_id]->mutable_gpu_diff(),0,sizeof(Dtype)*(net_params[param_id]->count()));
					}
		   			for(int i=0;i<this->childProcessSum;++i){
						if(this->flagComputeEndNeedUpdate[i]==1){
							for(int param_id = 0; param_id < net_params.size(); ++param_id){
								caffe_gpu_axpy(net_params[param_id]->count(),(Dtype)1,
										this->tempdata[i][param_id]->mutable_gpu_data(),
										net_params[param_id]->mutable_gpu_diff());
							}
						}
					}
			      
					for (int param_id = 0; param_id < net_params.size(); ++param_id) {	
						caffe_gpu_scal(net_params[param_id]->count(),(Dtype)(1.0/currentUpdateCount),net_params[param_id]->mutable_gpu_diff());
						Dtype local_rate = rate * net_params_lr[param_id];
						Dtype local_decay = weight_decay * net_params_weight_decay[param_id];
						if (local_decay) {
							if (regularization_type == "L2") {
								caffe_gpu_axpy(net_params[param_id]->count(),
										local_decay,
										net_params[param_id]->gpu_data(),
										net_params[param_id]->mutable_gpu_diff());
							} else if (regularization_type == "L1") {
								caffe_gpu_sign(net_params[param_id]->count(),
										net_params[param_id]->gpu_data(),
										temp_[param_id]->mutable_gpu_data());
								caffe_gpu_axpy(net_params[param_id]->count(),
										local_decay,
										temp_[param_id]->gpu_data(),
										net_params[param_id]->mutable_gpu_diff());
							} else {
								LOG(FATAL) << "Unknown regularization type: " << regularization_type;
							}
						}
						caffe_gpu_axpby(net_params[param_id]->count(), local_rate,
								net_params[param_id]->gpu_diff(), momentum,
								history_[param_id]->mutable_gpu_data());
						caffe_copy(net_params[param_id]->count(),
								history_[param_id]->gpu_data(),
								net_params[param_id]->mutable_gpu_diff());
						
			}
#else
					NO_GPU;
#endif
					break;
				default:
					LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
			}
			
			this->net_->Update();
           			for(int i=0;i<this->childProcessSum;++i){
				if(this->flagComputeEndNeedUpdate[i]==1){
					for (int param_id = 0; param_id < net_params.size(); ++param_id) {
						caffe_copy(net_params[param_id]->count(),
								net_params[param_id]->gpu_data(),
								this->tempdata[i][param_id]->mutable_gpu_data());
					}
				}

			}

		}





template<typename Dtype>
void Solver<Dtype>::SetActionFunction(ActionCallback func) {
  action_request_function_ = func;
}

template<typename Dtype>
SolverAction::Enum Solver<Dtype>::GetRequestedAction() {
  if (action_request_function_) {
    return action_request_function_();
  }
  return SolverAction::NONE;
}

template <typename Dtype>
Solver<Dtype>::Solver(const SolverParameter& param, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  Init(param);
}

template <typename Dtype>
Solver<Dtype>::Solver(const string& param_file, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  SolverParameter param;
  ReadSolverParamsFromTextFileOrDie(param_file, &param);
  Init(param);
}

template <typename Dtype>
void Solver<Dtype>::Init(const SolverParameter& param) {
  CHECK(Caffe::root_solver() || root_solver_)
      << "root_solver_ needs to be set for all non-root solvers";
  LOG_IF(INFO, Caffe::root_solver()) << "Initializing solver from parameters: "
    << std::endl << param.DebugString();
  param_ = param;
  CHECK_GE(param_.average_loss(), 1) << "average_loss should be non-negative.";
  CheckSnapshotWritePermissions();
  if (Caffe::root_solver() && param_.random_seed() >= 0) {
    Caffe::set_random_seed(param_.random_seed());
  }
  InitTrainNet();
 	int size;
  MPI_Comm_rank (MPI_COMM_WORLD, &rank);
  MPI_Comm_size (MPI_COMM_WORLD, &size);

  if (Caffe::root_solver()) {
    InitTestNets();
    LOG(INFO) << "Solver scaffolding done.";
  }
  iter_ = 0;
  current_step_ = 0;
}

	template <typename Dtype>
		void Solver<Dtype>::ComputeUpdateValueClientThread(int& mpi_source,int tid,int tid22){
			GetValue(mpi_source,tid,tid22);
		}
       template <typename Dtype>
                void Solver<Dtype>::ComputeUpdateValueClient() {
                        Dtype rate = GetLearningRate();
                        if (this->param_.display() && this->iter_ % this->param_.display() == 0) {
                                LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
                        }
                }


template <typename Dtype>
void Solver<Dtype>::InitTrainNet() {
  const int num_train_nets = param_.has_net() + param_.has_net_param() +
      param_.has_train_net() + param_.has_train_net_param();
  const string& field_names = "net, net_param, train_net, train_net_param";
  CHECK_GE(num_train_nets, 1) << "SolverParameter must specify a train net "
      << "using one of these fields: " << field_names;
  CHECK_LE(num_train_nets, 1) << "SolverParameter must not contain more than "
      << "one of these fields specifying a train_net: " << field_names;
  NetParameter net_param;
  if (param_.has_train_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in train_net_param.";
    net_param.CopyFrom(param_.train_net_param());
  } else if (param_.has_train_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from train_net file: " << param_.train_net();
    ReadNetParamsFromTextFileOrDie(param_.train_net(), &net_param);
  }
  if (param_.has_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in net_param.";
    net_param.CopyFrom(param_.net_param());
  }
  if (param_.has_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from net file: " << param_.net();
    ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
  }
  // Set the correct NetState.  We start with the solver defaults (lowest
  // precedence); then, merge in any NetState specified by the net_param itself;
  // finally, merge in any NetState specified by the train_state (highest
  // precedence).
  NetState net_state;
  net_state.set_phase(TRAIN);
  net_state.MergeFrom(net_param.state());
  net_state.MergeFrom(param_.train_state());
  net_param.mutable_state()->CopyFrom(net_state);
  if (Caffe::root_solver()) {
    net_.reset(new Net<Dtype>(net_param));
  } else {
    net_.reset(new Net<Dtype>(net_param, root_solver_->net_.get()));
  }
}

template <typename Dtype>
void Solver<Dtype>::InitTestNets() {
  CHECK(Caffe::root_solver());
  const bool has_net_param = param_.has_net_param();
  const bool has_net_file = param_.has_net();
  const int num_generic_nets = has_net_param + has_net_file;
  CHECK_LE(num_generic_nets, 1)
      << "Both net_param and net_file may not be specified.";
  const int num_test_net_params = param_.test_net_param_size();
  const int num_test_net_files = param_.test_net_size();
  const int num_test_nets = num_test_net_params + num_test_net_files;
  if (num_generic_nets) {
      CHECK_GE(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  } else {
      CHECK_EQ(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  }
  // If we have a generic net (specified by net or net_param, rather than
  // test_net or test_net_param), we may have an unlimited number of actual
  // test networks -- the actual number is given by the number of remaining
  // test_iters after any test nets specified by test_net_param and/or test_net
  // are evaluated.
  const int num_generic_net_instances = param_.test_iter_size() - num_test_nets;
  const int num_test_net_instances = num_test_nets + num_generic_net_instances;
  if (param_.test_state_size()) {
    CHECK_EQ(param_.test_state_size(), num_test_net_instances)
        << "test_state must be unspecified or specified once per test net.";
  }
  if (num_test_net_instances) {
    CHECK_GT(param_.test_interval(), 0);
  }
  int test_net_id = 0;
  vector<string> sources(num_test_net_instances);
  vector<NetParameter> net_params(num_test_net_instances);
  for (int i = 0; i < num_test_net_params; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net_param";
      net_params[test_net_id].CopyFrom(param_.test_net_param(i));
  }
  for (int i = 0; i < num_test_net_files; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net file: " + param_.test_net(i);
      ReadNetParamsFromTextFileOrDie(param_.test_net(i),
          &net_params[test_net_id]);
  }
  const int remaining_test_nets = param_.test_iter_size() - test_net_id;
  if (has_net_param) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net_param";
      net_params[test_net_id].CopyFrom(param_.net_param());
    }
  }
  if (has_net_file) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net file: " + param_.net();
      ReadNetParamsFromTextFileOrDie(param_.net(), &net_params[test_net_id]);
    }
  }
  test_nets_.resize(num_test_net_instances);
  for (int i = 0; i < num_test_net_instances; ++i) {
    // Set the correct NetState.  We start with the solver defaults (lowest
    // precedence); then, merge in any NetState specified by the net_param
    // itself; finally, merge in any NetState specified by the test_state
    // (highest precedence).
    NetState net_state;
    net_state.set_phase(TEST);
    net_state.MergeFrom(net_params[i].state());
    if (param_.test_state_size()) {
      net_state.MergeFrom(param_.test_state(i));
    }
    net_params[i].mutable_state()->CopyFrom(net_state);
    LOG(INFO)
        << "Creating test net (#" << i << ") specified by " << sources[i];
    if (Caffe::root_solver()) {
      test_nets_[i].reset(new Net<Dtype>(net_params[i]));
    } else {
      test_nets_[i].reset(new Net<Dtype>(net_params[i],
          root_solver_->test_nets_[i].get()));
    }
    test_nets_[i]->set_debug_info(param_.debug_info());
  }
}

template <typename Dtype>
void Solver<Dtype>::Step(int iters) {
  vector<Blob<Dtype>*> bottom_vec;
  const int start_iter = iter_;
  const int stop_iter = iter_ + iters;
  int average_loss = this->param_.average_loss();
  vector<Dtype> losses;
  Dtype smoothed_loss = 0;
  vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();


  while (iter_ < stop_iter) {
  	MPI_Status status;
  	status.MPI_ERROR=0;
  	
    // zero-init the params
    net_->ClearParamDiffs();
    if (param_.test_interval() && iter_ % param_.test_interval() == 0
        && (iter_ > 0 || param_.test_initialization())
        && Caffe::root_solver()) {
      TestAll();
      if (requested_early_exit_) {
        // Break out of the while loop because stop was requested while testing.
        break;
      }
    }

    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_start();
    }
    const bool display = param_.display() && iter_ % param_.display() == 0;
    net_->set_debug_info(display && param_.debug_info());
    // accumulate the loss and gradient
    Dtype loss = 0;
#ifdef ASYNCTRAN
    semRead.wait();
    DBGPRT(LOG(INFO)<<"FB START "<<i<<" "<<iter_);
    for (int i = 0; i < param_.iter_size(); ++i) {
      loss += net_->ForwardBackward(bottom_vec,&semNext);
    }
    DBGPRT(LOG(INFO)<<"FB FIN CUVC START "<<i<<" "<<iter_);
    semNext.notify();
#else
 		DBGPRT(LOG(INFO)<<"FB START "<<i<<" "<<iter_);    
 	  for (int i = 0; i < param_.iter_size(); ++i) {
      loss += net_->ForwardBackward(bottom_vec);
    }
    DBGPRT(LOG(INFO)<<"FB FIN CUVC START "<<i<<" "<<iter_);
#endif
    loss /= param_.iter_size();
    // average the loss across iterations for smoothed reporting
    if (losses.size() < average_loss) {
      losses.push_back(loss);
      int size = losses.size();
      smoothed_loss = (smoothed_loss * (size - 1) + loss) / size;
    } else {
      int idx = (iter_ - start_iter) % average_loss;
      smoothed_loss += (loss - losses[idx]) / average_loss;
      losses[idx] = loss;
    }
    if (display) {
      LOG_IF(INFO, Caffe::root_solver()) << "Iteration " << iter_
          << ", loss = " << smoothed_loss;
      const vector<Blob<Dtype>*>& result = net_->output_blobs();
      int score_index = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        const string& output_name =
            net_->blob_names()[net_->output_blob_indices()[j]];
        const Dtype loss_weight =
            net_->blob_loss_weights()[net_->output_blob_indices()[j]];
        for (int k = 0; k < result[j]->count(); ++k) {
          ostringstream loss_msg_stream;
          if (loss_weight) {
            loss_msg_stream << " (* " << loss_weight
                            << " = " << loss_weight * result_vec[k] << " loss)";
          }
          LOG_IF(INFO, Caffe::root_solver()) << "    Train net output #"
              << score_index++ << ": " << output_name << " = "
              << result_vec[k] << loss_msg_stream.str();
        }
      }
    }
    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_gradients_ready();
    }
    ApplyUpdate();

#ifdef DIRECTGPU
                                        for (int param_id = 0; param_id < net_params.size(); ++param_id) {
                                                caffe_mpi_recv<Dtype>(net_params[param_id]->mutable_gpu_data(),net_params[param_id]->count(),
                                                                0,TAG_NET_OUT,MPI_COMM_WORLD,&status);
                                        }
#else
                                        for (int param_id = 0; param_id < net_params.size(); ++param_id) {
                                                caffe_mpi_recv<Dtype>(net_params[param_id]->mutable_cpu_data(),net_params[param_id]->count(),
                                                                0,TAG_NET_OUT,MPI_COMM_WORLD,&status);
                                        }
#endif

  DBGPRT(LOG(INFO)<<"RECV NET FIN "<<i<<" "<<iter_);


    // Increment the internal iter_ counter -- its value should always indicate
    // the number of times the weights have been updated.
    ++iter_;

    SolverAction::Enum request = GetRequestedAction();

    // Save a snapshot if needed.
    if ((param_.snapshot()
         && iter_ % param_.snapshot() == 0
         && Caffe::root_solver()) ||
         (request == SolverAction::SNAPSHOT)) {
      Snapshot();
    }
    
//For MPI
//Added by ZhuHui 20151028    
#ifdef ASYNCTRAN
    threadReads.join();//////
#endif

    if (SolverAction::STOP == request) {
      requested_early_exit_ = true;
      // Break out of training loop.
      break;
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::Solve(const char* resume_file) {
  Caffe::set_phase(Caffe::TRAIN);
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Solving " << net_->name();
  LOG(INFO) << "Learning Rate Policy: " << param_.lr_policy();
  vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
  LOG(INFO) << "Learning Rate Policy: " <<net_params.size() ;
                        history_.clear();
                        update_.clear();
                        temp_.clear();
                        for (int i = 0; i < net_params.size(); ++i) {
                                const Blob<Dtype>* net_param = net_params[i].get();
                                history_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
                                                                net_param->num(), net_param->channels(), net_param->height(),
                                                                net_param->width())));
                                update_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
                                                                net_param->num(), net_param->channels(), net_param->height(),
                                                                net_param->width())));
                                temp_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
                                                                net_param->num(), net_param->channels(), net_param->height(),
                                                                net_param->width())));
                        }
  
// LOG(INFO)<<"net_params_size="<<net_params.size();
// LOG(INFO)<<"history_size="<<history_.size();
//LOG(INFO)<<"Caffe::mode()=="<<Caffe::mode();
 // LOG(INFO)<<"CPU=="<<Caffe::CPU<<" GPU=="<<Caffe::GPU;
  
  // Initialize to false every time we start solving.
  requested_early_exit_ = false;

  LOG(INFO) << "Learning Rate Policy: " <<net_params.size() ;
 iter_ = 0;

  if (resume_file) {
    LOG(INFO) << "Restoring previous solver status from " << resume_file;
    Restore(resume_file);
  }

  LOG(INFO) << "Learning Rate Policy: " <<net_params.size() ;
	int undone_iter=param_.max_iter() - iter_;
	vector<Blob<Dtype>*> bottom_vec;
	//vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
	int msize;
	MPI_Comm_size (MPI_COMM_WORLD, &msize);
	if(msize < 5)
		childProcessSum = msize -1 ;
	else if(msize < 10)
		childProcessSum = msize -2 ;
	else if(msize < 15)
		childProcessSum = msize -3 ;
	else
		childProcessSum = msize -4 ;
	undoneIter = param_.max_iter() - iter_;	
if(rank==0){
  LOG(INFO) << "Learning Rate Policy: " <<net_params.size() ;
	  flagComputeEndNeedUpdate=new int[childProcessSum]();
  	  tempdata= new Bloblite<Dtype> **[childProcessSum];
  	  for(int i=0;i<childProcessSum;++i){
    		tempdata[i]=new Bloblite<Dtype>* [net_params.size()];
    	  	for(int j=0;j<net_params.size();++j){
      			tempdata[i][j]= new Bloblite<Dtype>(net_params[j]->count());
    		}		
    	  }
}

	MPI_Barrier(MPI_COMM_WORLD);
if(rank==0){
    int itSize;
    itSize = param_.max_iter() / childProcessSum;
    int tailSize;
    tailSize = param_.max_iter() % childProcessSum;
    std::thread threadServer(&ComputeValueThreadServer<Dtype>,this,childProcessSum);
    vector<thread> threadClientUpdate(childProcessSum);
    int ii1 = 4<childProcessSum ? 4:childProcessSum;
    int ii2 = 8<childProcessSum ? 8:childProcessSum;
    int ii3 = 12<childProcessSum ? 12:childProcessSum;
    int ii4 = 16<childProcessSum ? 16:childProcessSum;
 	 for(int i=0;i< ii1;++i){
    		threadClientUpdate[i] = std::thread(&ComputeValueThreadClient<Dtype>,this,i+1,(i<tailSize ? itSize +1:itSize));
   	 }
 
  if(childProcessSum > 4)
    for(int i=ii1;i< ii2;++i){
    	threadClientUpdate[i] = std::thread(&ComputeValueThreadClient<Dtype>,this,i+2,(i<tailSize ? itSize +1:itSize));
    }
  if(childProcessSum > 8)
    for(int i=ii2;i< ii3;++i){
    	threadClientUpdate[i] = std::thread(&ComputeValueThreadClient<Dtype>,this,i+3,(i<tailSize ? itSize +1:itSize));
    }
  if(childProcessSum > 12)
       for(int i=ii3;i< ii4;++i){
    	threadClientUpdate[i] = std::thread(&ComputeValueThreadClient<Dtype>,this,i+4,(i<tailSize ? itSize +1:itSize));
    }
		vector<thread> threadRead(ii1);
   for(int i=0;i< ii1;++i){
    	threadRead[i] = std::thread(&ReadDataFromDBandDistribute<Dtype>,net_->layers().at(0).get(),
                                   i+1,(i<tailSize ? itSize +1:itSize),childProcessSum);
    }
    for(int i=0;i<ii1;++i){
      threadRead[i].join();
    }
    for(int i=0;i<childProcessSum;++i)
      threadClientUpdate[i].join();
    threadServer.join();
    threadClientUpdate.clear();
    threadRead.clear();
 }
else if(rank==5)
{
    int itSize;
    itSize = param_.max_iter() / childProcessSum;
    int tailSize;
    tailSize = param_.max_iter() % childProcessSum;
    int ii2 = 8<childProcessSum ? 4:childProcessSum-4;
   vector<thread> threadRead1(ii2);
   for(int i=0;i< ii2;++i){
    	threadRead1[i] = std::thread(&ReadDataFromDBandDistribute<Dtype>,net_->layers().at(0).get(),
                                   i+6,(i<tailSize ? itSize +1:itSize),childProcessSum);
    }
    for(int i=0;i<ii2;++i){
      threadRead1[i].join();
    }
	
}
else if(rank==10)
{
    int itSize;
    itSize = param_.max_iter() / childProcessSum;
    int tailSize;
    tailSize = param_.max_iter() % childProcessSum;
    int ii2 = 12<childProcessSum ? 4:childProcessSum-8;
   vector<thread> threadRead2(ii2);
   for(int i=0;i< ii2;++i){
    	threadRead2[i] = std::thread(&ReadDataFromDBandDistribute<Dtype>,net_->layers().at(0).get(),
                                   i+11,(i<tailSize ? itSize +1:itSize),childProcessSum);
    }
    for(int i=0;i<ii2;++i){
      threadRead2[i].join();
    }
}	
else if(rank==15)
{
    int itSize;
    itSize = param_.max_iter() / childProcessSum;
    int tailSize;
    tailSize = param_.max_iter() % childProcessSum;
    int ii2 = 16<childProcessSum ? 4:childProcessSum-12;
   vector<thread> threadRead3(ii2);
   for(int i=0;i< ii2;++i){
    	threadRead3[i] = std::thread(&ReadDataFromDBandDistribute<Dtype>,net_->layers().at(0).get(),
                                   i+16,(i<tailSize ? itSize +1:itSize),childProcessSum);
    }
    for(int i=0;i<ii2;++i){
      threadRead3[i].join();
    }
}	
else{// slave processes
   	int iterSize;
	iterSize = param_.max_iter() / childProcessSum;
	int tailtSize;
	tailtSize = param_.max_iter() % childProcessSum;
	int startNum,endNum;
	startNum = 0;
	endNum = iterSize;
	if(tailtSize >= rank) ++endNum;
	for(int i=startNum; i< endNum; ++i){
		MPI_Status status;
		status.MPI_ERROR=0;
		
if(rank<10)
{
	if(rank <5) 
			iter_ = (rank - 1) + i * childProcessSum;
	else
			iter_ = (rank - 2) + i * childProcessSum;
}
else 
{
	if(rank<15)
			iter_ = (rank - 3) + i * childProcessSum;
	else
			iter_ = (rank - 4) + i * childProcessSum;
}
		net_->taskiter = iter_;
		DBGPRT(LOG(INFO)<<"FB START "<<i<<" "<<iter_);
	        net_->ClearParamDiffs();
		Dtype loss = net_->ForwardBackward(bottom_vec);
		DBGPRT(LOG(INFO)<<"FB FIN CUVC START "<<i<<" "<<iter_);
		ComputeUpdateValueClient();
		DBGPRT(LOG(INFO)<<"CUVC FIN RECV NET START "<<i<<" "<<iter_);
		vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
#ifdef DIRECTGPU
		for (int param_id = 0; param_id < net_params.size(); ++param_id) {
			caffe_mpi_recv<Dtype>(net_params[param_id]->mutable_gpu_data(),net_params[param_id]->count(),
					0,TAG_NET_OUT,MPI_COMM_WORLD,&status);
			}
#else
		for (int param_id = 0; param_id < net_params.size(); ++param_id) {
			caffe_mpi_recv<Dtype>(net_params[param_id]->mutable_cpu_data(),net_params[param_id]->count(),
					0,TAG_NET_OUT,MPI_COMM_WORLD,&status);
			}
#endif
		DBGPRT(LOG(INFO)<<"RECV NET FIN "<<i<<" "<<iter_);
                //LOG(INFO)<<param_.display();		
		// Save a snapshot if needed.
		if (param_.snapshot() && iter_ > startNum &&
							iter_ % param_.snapshot() == 0) {
						Snapshot();
					}
					const bool display = param_.display() && iter_ % param_.display() == 0;
					net_->set_debug_info(display && param_.debug_info());
					if (display) {
						LOG(INFO) << "Iteration " << iter_ << ", loss = " << loss;
						const vector<Blob<Dtype>*>& result = net_->output_blobs();
						int score_index = 0;
						for (int j = 0; j < result.size(); ++j) {
							const Dtype* result_vec = result[j]->cpu_data();
							const string& output_name =
								net_->blob_names()[net_->output_blob_indices()[j]];
							const Dtype loss_weight =
								net_->blob_loss_weights()[net_->output_blob_indices()[j]];
							for (int k = 0; k < result[j]->count(); ++k) {
								ostringstream loss_msg_stream;
								if (loss_weight) {
									loss_msg_stream << " (* " << loss_weight
										<< " = " << loss_weight * result_vec[k] << " loss)";
								}
								LOG(INFO) << "    Train net output #"
									<< score_index++ << ": " << output_name << " = "
									<< result_vec[k] << loss_msg_stream.str();
							}
						}
					}
				}
	}  
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0){

  	if (param_.snapshot_after_train()
    	  && (!param_.snapshot() || iter_ % param_.snapshot() != 0)) {
    	Snapshot();
  	}
  	if (requested_early_exit_) {
    	LOG(INFO) << "Optimization stopped early.";
    	return;
  	}
  	// After the optimization is done, run an additional train and test pass to
  	// display the train and test loss/outputs if appropriate (based on the
  	// display and test_interval settings, respectively).  Unlike in the rest of
  	// training, for the train net we only run a forward pass as we've already
  	// updated the parameters "max_iter" times -- this final pass is only done to
  	// display the loss, which is computed in the forward pass.
  	if (param_.display() && iter_ % param_.display() == 0) {
    	Dtype loss;
    	//net_->ForwardPrefilled(&loss);
    	net_->taskiter = 0;
    	net_->ForwardTest(bottom_vec, &loss);
	LOG(INFO) << "Iteration " << iter_ << ", loss = " << loss;
  	}
  	if (param_.test_interval() && iter_ % param_.test_interval() == 0) {
    	TestAll();
  	}
  	LOG(INFO) << "Optimization Done.";
	}
}

template <typename Dtype>
void Solver<Dtype>::TestAll() {
  for (int test_net_id = 0;
       test_net_id < test_nets_.size() && !requested_early_exit_;
       ++test_net_id) {
    Test(test_net_id);
  }
}

template <typename Dtype>
void Solver<Dtype>::Test(const int test_net_id) {
  Caffe::set_phase(Caffe::TEST);
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Iteration " << iter_
            << ", Testing net (#" << test_net_id << ")";
  CHECK_NOTNULL(test_nets_[test_net_id].get())->
      ShareTrainedLayersWith(net_.get());
  vector<Dtype> test_score;
  vector<int> test_score_output_id;
  vector<Blob<Dtype>*> bottom_vec;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    SolverAction::Enum request = GetRequestedAction();
    // Check to see if stoppage of testing/training has been requested.
    
    while (request != SolverAction::NONE) {
        if (SolverAction::SNAPSHOT == request) {
          Snapshot();
        } else if (SolverAction::STOP == request) {
          requested_early_exit_ = true;
        }
        request = GetRequestedAction();
    }
    if (requested_early_exit_) {
      // break out of test loop.
      break;
    }

    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
       // test_net->Forward(bottom_vec, &iter_loss);
          test_net->ForwardTest(bottom_vec, &iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }

    if (i == 0) {
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score.push_back(result_vec[k]);
          test_score_output_id.push_back(j);
        }
      }
    } else {
      int idx = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score[idx++] += result_vec[k];
        }
      }
    }
  }
  


  if (requested_early_exit_) {
    LOG(INFO)     << "Test interrupted.";
    return;
  }
  if (param_.test_compute_loss()) {
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
  }
  

  for (int i = 0; i < test_score.size(); ++i) {
    const int output_blob_index =
        test_net->output_blob_indices()[test_score_output_id[i]];
    const string& output_name = test_net->blob_names()[output_blob_index];
    const Dtype loss_weight = test_net->blob_loss_weights()[output_blob_index];
    ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / param_.test_iter(test_net_id);
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mean_score << loss_msg_stream.str();
  }
  Caffe::set_phase(Caffe::TRAIN);
}

template <typename Dtype>
void Solver<Dtype>::Snapshot() {
  CHECK(Caffe::root_solver());
  string model_filename;
  switch (param_.snapshot_format()) {
  case caffe::SolverParameter_SnapshotFormat_BINARYPROTO:
    model_filename = SnapshotToBinaryProto();
    break;
  case caffe::SolverParameter_SnapshotFormat_HDF5:
    model_filename = SnapshotToHDF5();
    break;
  default:
    LOG(FATAL) << "Unsupported snapshot format.";
  }

  SnapshotSolverState(model_filename);
}

template <typename Dtype>
void Solver<Dtype>::CheckSnapshotWritePermissions() {
  if (Caffe::root_solver() && param_.snapshot()) {
    CHECK(param_.has_snapshot_prefix())
        << "In solver params, snapshot is specified but snapshot_prefix is not";
    string probe_filename = SnapshotFilename(".tempfile");
    std::ofstream probe_ofs(probe_filename.c_str());
    if (probe_ofs.good()) {
      probe_ofs.close();
      std::remove(probe_filename.c_str());
    } else {
      LOG(FATAL) << "Cannot write to snapshot prefix '"
          << param_.snapshot_prefix() << "'.  Make sure "
          << "that the directory exists and is writeable.";
    }
  }
}

template <typename Dtype>
string Solver<Dtype>::SnapshotFilename(const string extension) {
  string filename(param_.snapshot_prefix());
  const int kBufferSize = 20;
  char iter_str_buffer[kBufferSize];
  snprintf(iter_str_buffer, kBufferSize, "_iter_%d", iter_);
  return filename + iter_str_buffer + extension;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToBinaryProto() {
  string model_filename = SnapshotFilename(".caffemodel");
  LOG(INFO) << "Snapshotting to binary proto file " << model_filename;
  NetParameter net_param;
  net_->ToProto(&net_param, param_.snapshot_diff());
  WriteProtoToBinaryFile(net_param, model_filename);
  return model_filename;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToHDF5() {
  string model_filename = SnapshotFilename(".caffemodel.h5");
  LOG(INFO) << "Snapshotting to HDF5 file " << model_filename;
  net_->ToHDF5(model_filename, param_.snapshot_diff());
  return model_filename;
}

template <typename Dtype>
void Solver<Dtype>::Restore(const char* state_file) {
  CHECK(Caffe::root_solver());
  string state_filename(state_file);
  if (state_filename.size() >= 3 &&
      state_filename.compare(state_filename.size() - 3, 3, ".h5") == 0) {
    RestoreSolverStateFromHDF5(state_filename);
  } else {
    RestoreSolverStateFromBinaryProto(state_filename);
  }
}


	template <typename Dtype>
		void Solver<Dtype>::GetValue(int &mpi_source,const int tid,int tid22) {
			MPI_Status status;
			vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
			for (int param_id = net_params.size()-1; param_id >= 0; --param_id) {
				memset(&status,0,sizeof(status));
				
#ifdef DIRECTGPU
					caffe_mpi_recv<Dtype>(this->tempdata[tid22-1][param_id]->mutable_gpu_data(),net_params[param_id]->count(),
							tid,TAG_UPDATE,MPI_COMM_WORLD,&status);
#else
					caffe_mpi_recv<Dtype>(this->tempdata[tid22-1][param_id]->mutable_cpu_data(),net_params[param_id]->count(),
							tid,TAG_UPDATE,MPI_COMM_WORLD,&status);
#endif
			}
			{
				unique_lock<mutex> lk(mutexUpdateWeight);
				this->flagComputeEndNeedUpdate[tid22-1] = 1;
				++currentUpdateCount;
				condUpdateWeight.notify_all();
			}
		}

       template <typename Dtype>
                Solver<Dtype>::~Solver(){
                        if(this->rank==0){
                                delete [] this->flagComputeEndNeedUpdate;
                                vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params_nc();
                                for(int i=0;i<this->childProcessSum;++i){
                                        for(int j=0;j<net_params.size();++j)
                                                delete this->tempdata[i][j];
                                        delete[] this->tempdata[i];
                                }
                                delete [] this->tempdata;
                        }else{
                        }
                }

INSTANTIATE_CLASS(Solver);

}  // namespace caffe
