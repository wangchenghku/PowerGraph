/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */


#ifndef GRAPHLAB_DISTRIBUTED_AGGREGATOR
#define GRAPHLAB_DISTRIBUTED_AGGREGATOR

#ifndef __NO_OPENMP__
#include <omp.h>
#endif

#include <map>
#include <set>
#include <string>
#include <vector>
#include <graphlab/rpc/dc_dist_object.hpp>
#include <graphlab/vertex_program/icontext.hpp>
#include <graphlab/graph/distributed_graph.hpp>
#include <graphlab/util/generics/conditional_addition_wrapper.hpp>
#include <graphlab/util/generics/any.hpp>
#include <graphlab/util/timer.hpp>
#include <graphlab/util/mutable_queue.hpp>
#include <graphlab/logger/assertions.hpp>
#include <graphlab/parallel/pthread_tools.hpp>
#include <graphlab/macros_def.hpp>
namespace graphlab {

  /**
   * \internal
   * Implements a distributed aggregator interface which can be plugged
   * into the engine. This class includes management of periodic aggregators.
   * 
   * Essentially, the engine should ideally pass-through all calls to
   *  - add_vertex_aggregator()
   *  - add_edge_aggregator()
   *  - aggregate_now()
   *  - aggregate_periodic()
   * 
   * On engine start(), the engine should call aggregate_all_periodic() 
   * to ensure all periodic aggregators are called once prior to vertex program
   * execution. After which, the start() function should be called to prepare
   * the state of the schedule. At termination of the engine, the stop()
   * function should be called to reset the state of the aggregator.
   * 
   * During engine execution, two modes of operations are permitted: 
   * synchronous, and asynchronous. In a synchronous mode of execution,
   * the tick_synchronous() function should be called periodically by 
   * exactly one thread on each machine, at the same time. In an asynchronous
   * mode of execution, tick_asynchronous() should be called periodically
   * on each machine by some arbitrary thread. This polls the state of the 
   * schedule and activates aggregation jobs which are ready. 
   * 
   * tick_synchronous() and tick_asynchronous() should not be used 
   * simultaneously within the same engine execution . For details on their 
   * usage, see their respective documentation.
   * 
   */
  template<typename Graph, typename IContext>
  class distributed_aggregator {
  public:
    typedef Graph graph_type;
    typedef typename graph_type::local_edge_list_type local_edge_list_type;
    typedef typename graph_type::local_edge_type local_edge_type;
    typedef typename graph_type::edge_type edge_type;
    typedef typename graph_type::local_vertex_type local_vertex_type;
    typedef typename graph_type::vertex_type vertex_type ;
    typedef IContext icontext_type;

    dc_dist_object<distributed_aggregator> rmi;
    graph_type& graph;
    icontext_type* context;
    
  private:
    
    /**
     * \internal
     * A base class which contains a "type-free" specification of the
     * reduction operation, thus allowing the aggregation to be performs at
     * runtime with no other type information whatsoever.
     */
    struct imap_reduce_base {
      /** \brief makes a copy of the current map reduce spec without copying 
       *         accumulator data     */
      virtual imap_reduce_base* clone_empty() const = 0;
      
      /** \brief Performs a map operation on the given vertex adding to the
       *         internal accumulator */
      virtual void perform_map_vertex(icontext_type&,
                                      vertex_type&) = 0;
                                      
      /** \brief Performs a map operation on the given edge adding to the
       *         internal accumulator */
      virtual void perform_map_edge(icontext_type&,
                                    edge_type&) = 0;
                                    
      /** \brief Returns true if the accumulation is over vertices. 
                 Returns false if it is over edges.*/
      virtual bool is_vertex_map() const = 0;      
      
      /** \brief Returns the accumulator stored in an any. 
                 (by some magic, any's can be serialized) */
      virtual any get_accumulator() const = 0;
      
      /** \brief Combines accumulators using a second accumulator 
                 stored in an any (as returned by get_accumulator).
                 Must be thread safe.*/
      virtual void add_accumulator_any(any& other) = 0;

      /** \brief Sets the value of the accumulator
                 from an any (as returned by get_accumulator).
                 Must be thread safe.*/
      virtual void set_accumulator_any(any& other) = 0;

      
      /** \brief Combines accumulators using a second accumulator 
                 stored in a second imap_reduce_base class). Must be
                 thread safe. */
      virtual void add_accumulator(imap_reduce_base* other) = 0;
      
      /** \brief Resets the accumulator */
      virtual void clear_accumulator() = 0;
      
      /** \brief Calls the finalize operation on internal accumulator */
      virtual void finalize(icontext_type&) = 0;

      virtual ~imap_reduce_base() { }
    };
    
    /**
     * \internal
     * A templated implementation of the imap_reduce_base above.
     * \tparam ReductionType The reduction type. (The type the map function
     *                        returns)
     */
    template <typename ReductionType>
    struct map_reduce_type : public imap_reduce_base {
      conditional_addition_wrapper<ReductionType> acc;
      boost::function<ReductionType(icontext_type&, vertex_type&)>
                                                            map_vtx_function;
      boost::function<ReductionType(icontext_type&, edge_type&)>
                                                            map_edge_function;
      boost::function<void(icontext_type&, const ReductionType&)>
                                                            finalize_function;
      
      bool vertex_map;
      mutex lock;
      
      /**
       * \brief Constructor which constructs a vertex reduction
       */
      map_reduce_type(
            boost::function<ReductionType(icontext_type&,
                                          vertex_type&)> map_vtx_function,
            boost::function<void(icontext_type&,
                                  const ReductionType&)> finalize_function)
                : map_vtx_function(map_vtx_function),
                  finalize_function(finalize_function), vertex_map(true) { }

      /**
       * \brief Constructor which constructs an edge reduction. The last bool
       * is unused and allows for disambiguation between the two constructors
       */
      map_reduce_type(
            boost::function<ReductionType(icontext_type&,
                                          edge_type&)> map_edge_function,
            boost::function<void(icontext_type&,
                                  const ReductionType&)> finalize_function,
            bool)
                : map_edge_function(map_edge_function),
                finalize_function(finalize_function), vertex_map(false) { }


      void perform_map_vertex(icontext_type& context, vertex_type& vertex) {
        acc += map_vtx_function(context, vertex);
      }
      
      void perform_map_edge(icontext_type& context, edge_type& vertex) {
        acc += map_edge_function(context, vertex);
      }
      
      bool is_vertex_map() const {
        return vertex_map;
      }
      
      any get_accumulator() const {
        return any(acc);
      }
      
      void add_accumulator_any(any& other) {
        lock.lock();
        acc += other.as<conditional_addition_wrapper<ReductionType> >();
        lock.unlock();
      }

      void set_accumulator_any(any& other) {
        lock.lock();
        acc = other.as<conditional_addition_wrapper<ReductionType> >();
        lock.unlock();
      }


      void add_accumulator(imap_reduce_base* other) {
        lock.lock();
        acc += dynamic_cast<map_reduce_type<ReductionType>*>(other)->acc;
        lock.unlock();
      }

      void clear_accumulator() {
        acc.clear();
      }

      void finalize(icontext_type& context) {
        finalize_function(context, acc.value);
      }
      
      imap_reduce_base* clone_empty() const {
        map_reduce_type<ReductionType>* copy;
        if (is_vertex_map()) {
          copy = new map_reduce_type<ReductionType>(map_vtx_function,
                                                    finalize_function);
        }
        else {
          copy = new map_reduce_type<ReductionType>(map_edge_function,
                                                    finalize_function,
                                                    true);
        }
        return copy;
      }
    };
    

    std::map<std::string, imap_reduce_base*> aggregators;
    std::map<std::string, float> aggregate_period;

    struct async_aggregator_state {
      /// Performs reduction of all local threads. On machine 0, also
      /// accumulates for all machines.
      imap_reduce_base* root_reducer;
      /// Accumulator used for each thread
      std::vector<imap_reduce_base*> per_thread_aggregation;
      /// Count down the completion of the local machine threads
      atomic<int> local_count_down;
      /// Count down the completion of machines. Used only on machine 0
      atomic<int> distributed_count_down;
    };
    std::map<std::string, async_aggregator_state> async_state;

    float start_time;
    
    /* annoyingly the mutable queue is a max heap when I need a min-heap
     * to track the next thing to activate. So we need to keep 
     *  negative priorities... */
    mutable_queue<std::string, float> schedule;
    mutex schedule_lock;
    size_t ncpus;
  public:

    
    distributed_aggregator(distributed_control& dc, 
                           graph_type& graph, 
                           icontext_type* context):
                            rmi(dc, this), graph(graph), 
                            context(context), ncpus(0) { }

    /** 
     * \brief Creates a vertex aggregator. Returns true on success.
     *        Returns false if an aggregator of the same name already
     *        exists.
     *
     * Creates a vertex aggregator associated to a particular key.
     * The map_function is called over every vertex in the graph, and the
     * return value of the map is summed. The finalize_function is then called
     * on the result of the reduction.
     *
     * \tparam ReductionType The output of the map function. Must be summable
     *                       and \ref Serializable.
     * \param [in] map_function The Map function to use. Must take an
     *                          icontext_type& as its first argument, and an
     *                          vertex_type& as its second argument. Returns a
     *                          ReductionType which must be summable and
     *                          \ref Serializable
     * \param [in] finalize_function The Finalize function to use. Must take
     *                               an icontext_type& as its first argument
     *                               and a const ReductionType& as its second
     *                               argument.
     *
     * \warning Pay attention to the types! A slightly erroneous type
     *          can produce screens of errors.
     */
    template <typename ReductionType>
    bool add_vertex_aggregator(const std::string& key,
      boost::function<ReductionType(icontext_type&,
                                    vertex_type&)> map_function,
      boost::function<void(icontext_type&,
                           const ReductionType&)> finalize_function) {
      if (key.length() == 0) return false;
      if (aggregators.count(key) == 0) {
        aggregators[key] = new map_reduce_type<ReductionType>(map_function, 
                                                              finalize_function);
        return true;
      }
      else {
        // aggregator already exists. fail 
        return false;
      }
    }
    
    
    /** \brief Creates a edge aggregator. Returns true on success.
     *         Returns false if an aggregator of the same name already exists
     *
     * Creates an edge aggregator associated to a particular key.
     * The map_function is called over every edge in the graph, and the return
     * value of the map is summed. The finalize_function is then called on
     * the result of the reduction.
     * 
     * \tparam ReductionType The output of the map function. Must be summable
     *                       and \ref Serializable.
     * \param [in] map_function The Map function to use. Must take an
     *                          icontext_type& as its first argument, and an
     *                          edge_type& as its second argument. Returns a
     *                          ReductionType which must be summable and
     *                          \ref Serializable
     * \param [in] finalize_function The Finalize function to use. Must take
     *                               an icontext_type& as its first argument
     *                               and a const ReductionType& as its second
     *                               argument.
     *
     * \warning Pay attention to the types! A slightly erroneous type
     *          can produce screens of errors
     */
    template <typename ReductionType>
    bool add_edge_aggregator(const std::string& key,
      boost::function<ReductionType(icontext_type&,
                                    edge_type&)> map_function,
      boost::function<void(icontext_type&,
                           const ReductionType&)> finalize_function) {
      if (key.length() == 0) return false;
      if (aggregators.count(key) == 0) {
        aggregators[key] = new map_reduce_type<ReductionType>(map_function, 
                                                              finalize_function, 
                                                              true);
        return true;
      }
      else {
        // aggregator already exists. fail 
        return false;
      }
    }
    
    
    /**
     * Performs an immediate aggregation on a key. All machines must
     * call this simultaneously. If the key is not found,
     * false is returned. Otherwise return true on success.
     *
     * \param[in] key Key to aggregate now
     * \return False if key not found, True on success.
     */
    bool aggregate_now(const std::string& key) {
      if (aggregators.count(key) == 0) {
        ASSERT_MSG(false, "Requested aggregator %s not found", key.c_str());
        return false;
      }
      
      imap_reduce_base* mr = aggregators[key];
      mr->clear_accumulator();
      // ok. now we perform reduction on local data in parallel
#ifdef _OPENMP
#pragma omp parallel
#endif
      {
        imap_reduce_base* localmr = mr->clone_empty();
        if (localmr->is_vertex_map()) {
#ifdef _OPENMP
        #pragma omp for
#endif
          for (int i = 0;i < (int)graph.num_local_vertices(); ++i) {
            local_vertex_type lvertex = graph.l_vertex(i);
            if (lvertex.owner() == rmi.procid()) {
              vertex_type vertex(lvertex);
              localmr->perform_map_vertex(*context, vertex);
            }
          }
        }
        else {
          for (int i = 0;i < (int)graph.num_local_vertices(); ++i) {
            foreach(local_edge_type e, graph.l_vertex(i).in_edges()) {
              edge_type edge(e);
              localmr->perform_map_edge(*context, edge);
            }
          }
        }
#ifdef _OPENMP
        #pragma omp critical
#endif
        {
          mr->add_accumulator(localmr);
        }
        delete localmr;
      }
      
      std::vector<any> gathervec(rmi.numprocs());
      gathervec[rmi.procid()] = mr->get_accumulator();
      
      rmi.gather(gathervec, 0);
      
      if (rmi.procid() == 0) {
        // machine 0 aggregates the accumulators
        // sums them together and broadcasts it
        for (procid_t i = 1; i < rmi.numprocs(); ++i) {
          mr->add_accumulator_any(gathervec[i]);
        }
        any val = mr->get_accumulator();
        rmi.broadcast(val, true);
      }
      else {
        // all other machines wait for the broadcast value
        any val;
        rmi.broadcast(val, false);
        mr->set_accumulator_any(val);
      }
      mr->finalize(*context);
      mr->clear_accumulator();
      gathervec.clear();
      return true;
    }
    
    
    /**
     * Requests that the aggregator with a given key be aggregated
     * every certain number of seconds when the engine is running.
     * Note that the period is prescriptive: in practice the actual
     * period will be larger than the requested period. 
     * Seconds must be >= 0;
     *
     * \param [in] key Key to schedule
     * \param [in] seconds How frequently to schedule. Must be >= 0. In the
     *                     synchronous engine, seconds == 0 will ensure that
     *                     this key is recomputed every iteration.
     * 
     * All machines must call simultaneously.
     * \return Returns true if key is found and seconds >= 0,
     *         and false otherwise.
     */
    bool aggregate_periodic(const std::string& key, float seconds) {
      rmi.barrier();
      if (seconds < 0) return false;
      if (aggregators.count(key) == 0) return false;
      else aggregate_period[key] = seconds;
      return true;
    }
    
    /**
     * Performs aggregation on all keys registered with a period.
     * May be used on engine start() to ensure all periodic 
     * aggregators are executed before engine execution.
     */
    void aggregate_all_periodic() {
      typename std::map<std::string, float>::iterator iter =
                                                    aggregate_period.begin();
      while (iter != aggregate_period.end()) { 
        aggregate_now(iter->first);
        ++iter;
      }
    }
    
    
    /**
     * Must be called on engine start. Initializes the internal scheduler.
     * Must be called on all machines simultaneously.
     * ncpus is really only important for the asynchronous implementation.
     * It must be equal to the number of engine threads.
     *
     * \param [in] cpus Number of engine threads used. This is only necessary
     *                  if the asynchronous form is used.
     */
    void start(size_t ncpus = 0) {
      rmi.barrier();
      schedule.clear();
      start_time = timer::approx_time_seconds();
      typename std::map<std::string, float>::iterator iter =
                                                    aggregate_period.begin();
      while (iter != aggregate_period.end()) {
        // schedule is a max heap. To treat it like a min heap
        // I need to insert negative keys
        schedule.push(iter->first, -iter->second);
        ++iter;
      }
      this->ncpus = ncpus;

      // now initialize the asyncronous reduction states
      if(ncpus > 0) {
        iter = aggregate_period.begin();
        while (iter != aggregate_period.end()) {
          async_state[iter->first].local_count_down = (int)ncpus;
          async_state[iter->first].distributed_count_down =
                                                        (int)rmi.numprocs();
          
          async_state[iter->first].per_thread_aggregation.resize(ncpus);
          for (size_t i = 0;i < ncpus; ++i) {
            async_state[iter->first].per_thread_aggregation[i] =
                                    aggregators[iter->first]->clone_empty();
          }
          async_state[iter->first].root_reducer =
                                      aggregators[iter->first]->clone_empty();
          ++iter;
        }
      }
    }
    
    
    /**
     * If asynchronous aggregation is desired, this function is
     * to be called periodically on each machine. This polls the schedule to
     * check if there is an aggregator which needs to be activated. If there
     * is an aggregator to be started, this function will return a non empty
     * string. This function is thread reentrant and each activated aggregator
     * will only return a non empty string call to one call to
     * tick_asynchronous() on each machine.
     * 
     * If an empty is returned, the asynchronous engine
     * must ensure that all threads (ncpus per machine) must eventually
     * call tick_asynchronous_compute(cpuid, key) where i is the return string.
     */ 
    std::string tick_asynchronous() {
      // if we fail to acquire the lock, go ahead
      if (!schedule_lock.try_lock()) return "";
      
      // see if there is a key to run
      float curtime = timer::approx_time_seconds() - start_time;
      std::string key;
      bool has_entry = false;
      if (!schedule.empty() && -schedule.top().second < curtime) {
        key = schedule.top().first;
        has_entry = true;
        schedule.pop();
      }
      schedule_lock.unlock();

      // no key to run. return false
      if (has_entry == false) return "";
      else return key;
      // ok. we have a key to run, construct the local reducers
    }

    
    /**
     * Once tick_asynchronous() returns a key, all in the engine
     * should call tick_asynchronous_compute with a matching key.
     * This function will perform the computation for the key in question
     * and send the accumulated result back to machine 0 when done
     */
    void tick_asynchronous_compute(size_t cpuid, const std::string& key) {
      // acquire and check the async_aggregator_state
      typename std::map<std::string, async_aggregator_state>::iterator iter =
                                                        async_state.find(key);
      ASSERT_MSG(iter != async_state.end(), "Key %s not found", key.c_str());
      ASSERT_GT(iter->second.per_thread_aggregation.size(), cpuid);
      
      imap_reduce_base* localmr = iter->second.per_thread_aggregation[cpuid];
      // perform the reduction using the local mr
      if (localmr->is_vertex_map()) {
        for (int i = cpuid;i < (int)graph.num_local_vertices(); i+=ncpus) {
          local_vertex_type lvertex = graph.l_vertex(i);
          if (lvertex.owner() == rmi.procid()) {
            vertex_type vertex(lvertex);
            localmr->perform_map_vertex(*context, vertex);
          }
        }
      } else {
        for (int i = cpuid;i < (int)graph.num_local_vertices(); i+=ncpus) {
          foreach(local_edge_type e, graph.l_vertex(i).in_edges()) {
            edge_type edge(e);
            localmr->perform_map_edge(*context, edge);
          }
        }
      }
      iter->second.root_reducer->add_accumulator(localmr);
      int countdown_val = iter->second.local_count_down.dec();

      ASSERT_LT(countdown_val, ncpus);
      ASSERT_GE(countdown_val, 0);
      if (countdown_val == 0) {
        // reset the async_state to pristine condition.
        // - clear all thread reducers since we got all we need from them
        // - clear all the local root reducer except for machine 0 (and after
        //   we read the accumulator from them.
        // - reset the counters
        for (size_t i = 0;
             i < iter->second.per_thread_aggregation.size(); ++i) {
          iter->second.per_thread_aggregation[i]->clear_accumulator();
        }
        iter->second.local_count_down = ncpus;
        
        if (rmi.procid() != 0) {
          // ok we need to signal back to the the root to perform finalization
          // read the accumulator
          any acc = iter->second.root_reducer->get_accumulator();
          iter->second.root_reducer->clear_accumulator();
          rmi.remote_call(0, &distributed_aggregator::rpc_key_merge,
                          key, acc);
        }
        else {
          decrement_distributed_counter(key);
        }
      }
    }

    /**
     * RPC Call called by other machines with their accumulator for the key.
     * This function will merge the accumulator and perform finalization
     * when all accumulators are received
     */
    void rpc_key_merge(const std::string& key, any& acc) {
      // acquire and check the async_aggregator_state 
      typename std::map<std::string, async_aggregator_state>::iterator iter =
                                                      async_state.find(key);
      ASSERT_MSG(iter != async_state.end(), "Key %s not found", key.c_str());
      iter->second.root_reducer->add_accumulator_any(acc);
      decrement_distributed_counter(key);
    }

    /**
     * Called whenever one machine finishes all of its local accumulation.
     * When the counter determines that all machine's accumulators have been
     * received, this function performs finalization and prepares and
     * broadcasts the next scheduled time for the key.
     */
    void decrement_distributed_counter(const std::string& key) {
      // must be master machine
      ASSERT_EQ(rmi.procid(), 0);
      // acquire and check the async_aggregator_state 
      typename std::map<std::string, async_aggregator_state>::iterator iter =
                                                      async_state.find(key);
      ASSERT_MSG(iter != async_state.end(), "Key %s not found", key.c_str());
      int countdown_val = iter->second.distributed_count_down.dec();
      logstream(LOG_INFO) << "Distributed Aggregation of " << key << ". "
                          << countdown_val << " remaining." << std::endl;

      ASSERT_LE(countdown_val, rmi.numprocs());
      ASSERT_GE(countdown_val, 0);
      if (countdown_val == 0) {
        logstream(LOG_INFO) << "Aggregate completion of " << key << std::endl;
        any acc_val = iter->second.root_reducer->get_accumulator();
        // set distributed count down again for the second phase:
        // waiting for everyone to finish finalization
        iter->second.distributed_count_down = rmi.numprocs();
        for (procid_t i = 1;i < rmi.numprocs(); ++i) {
          rmi.remote_call(i, &distributed_aggregator::rpc_perform_finalize,
                            key, acc_val);
        }
        iter->second.root_reducer->finalize(*context);
        iter->second.root_reducer->clear_accumulator();
        decrement_finalize_counter(key);
      }
    }

    /**
     * Called from the root machine to all machines to perform finalization
     * on the key
     */
    void rpc_perform_finalize(const std::string& key, any& acc_val) {
      ASSERT_NE(rmi.procid(), 0);
      typename std::map<std::string, async_aggregator_state>::iterator iter =
                                                  async_state.find(key);
      ASSERT_MSG(iter != async_state.end(), "Key %s not found", key.c_str());
      
      iter->second.root_reducer->set_accumulator_any(acc_val);
      iter->second.root_reducer->finalize(*context);
      iter->second.root_reducer->clear_accumulator();
      // reply to the root machine
      rmi.remote_call(0, &distributed_aggregator::decrement_finalize_counter,
                      key);
    }


    void decrement_finalize_counter(const std::string& key) {
      typename std::map<std::string, async_aggregator_state>::iterator iter =
                                                      async_state.find(key);
      ASSERT_MSG(iter != async_state.end(), "Key %s not found", key.c_str());
      int countdown_val = iter->second.distributed_count_down.dec();
      if (countdown_val == 0) {
        // done! all finalization is complete.
        // reset the counter
        iter->second.distributed_count_down = rmi.numprocs();
        // when is the next time we start. 
        // time is as an offset to start_time
        float next_time = timer::approx_time_seconds() + 
                          aggregate_period[key] - start_time;
        logstream(LOG_INFO) << rmi.procid() << "Reschedule of " << key
                          << " at " << next_time << std::endl;
        rpc_schedule_key(key, next_time);
        for (procid_t i = 1;i < rmi.numprocs(); ++i) {
          rmi.remote_call(i, &distributed_aggregator::rpc_schedule_key,
                            key, next_time);
        }
      }
    }

    /**
     * Called to schedule the next trigger time for the key
     */
    void rpc_schedule_key(const std::string& key, float next_time) {
      schedule_lock.lock();
      schedule.push(key, -next_time);
      schedule_lock.unlock();
    }

    
    /**
     * If synchronous aggregation is desired, this function is
     * To be called simultaneously by one thread on each machine. 
     * This polls the schedule to see if there
     * is an aggregator which needs to be activated. If there is an aggregator 
     * to be started, this function will perform aggregation.
     */ 
    void tick_synchronous() {
      // if timer has exceeded our top key
      float curtime = timer::approx_time_seconds() - start_time;
      rmi.broadcast(curtime, rmi.procid() == 0);
      // note that we do not call approx_time_seconds everytime
      // this ensures that each key will only be run at most once.
      // each time tick_synchronous is called.
      while(!schedule.empty() && -schedule.top().second < curtime) {
        std::string key = schedule.top().first;
        aggregate_now(key);
        schedule.pop();
        // when is the next time we start. 
        // time is as an offset to start_time
        float next_time = (timer::approx_time_seconds() + 
                           aggregate_period[key] - start_time);
        rmi.broadcast(next_time, rmi.procid() == 0);
        schedule.push(key, -next_time);
      }
    }

    /**
     * Must be called on engine stop. Clears the internal scheduler
     * And resets all incomplete states.
     */
    void stop() {
      schedule.clear();
      // clear the aggregators
      {
        typename std::map<std::string, imap_reduce_base*>::iterator iter =
                                                          aggregators.begin();
        while (iter != aggregators.end()) {
          iter->second->clear_accumulator();
          ++iter;
        }
      }
      // clear the asynchronous state
      {
        typename std::map<std::string, async_aggregator_state>::iterator
                                                  iter = async_state.begin();
        while (iter != async_state.end()) {
          delete iter->second.root_reducer;
          for (size_t i = 0;
               i < iter->second.per_thread_aggregation.size();
               ++i) {
            delete iter->second.per_thread_aggregation[i];
          }
          iter->second.per_thread_aggregation.clear();
          ++iter;
        }
        async_state.clear();
      }
    }


    std::set<std::string> get_all_periodic_keys() const {
      typename std::map<std::string, float>::const_iterator iter =
                                                    aggregate_period.begin();
      std::set<std::string> ret;
      while (iter != aggregate_period.end()) {
        ret.insert(iter->first);
        ++iter;
      }
      return ret;
    }
    ~distributed_aggregator() {
      delete context;
    }
  }; 


}; // end of graphlab namespace
#include <graphlab/macros_undef.hpp>

#endif
