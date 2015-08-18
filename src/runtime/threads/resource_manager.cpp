//  Copyright (c) 2007-2013 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_fwd.hpp>
#include <hpx/include/threads.hpp>
#include <hpx/runtime/threads/thread_executor.hpp>
#include <hpx/runtime/threads/resource_manager.hpp>
#include <hpx/lcos/local/once.hpp>
#include <hpx/util/reinitializable_static.hpp>
#include <hpx/util/high_resolution_clock.hpp>

#include <boost/chrono/chrono.hpp>
#include <boost/thread/locks.hpp>

namespace hpx { namespace threads
    {
        ///////////////////////////////////////////////////////////////////////////
        resource_manager& resource_manager::get()
        {
            typedef util::reinitializable_static<resource_manager, tag> static_type;

            static_type instance;
            return instance.get();
        }

        ///////////////////////////////////////////////////////////////////////////
        resource_manager::resource_manager()
            : next_cookie_(0),
            punits_(get_os_thread_count()),
            topology_(get_topology())
        {}

        // Request an initial resource allocation
        std::size_t resource_manager::initial_allocation(
                detail::manage_executor* proxy, error_code& ec)
        {
            if (0 == proxy) {
                HPX_THROWS_IF(ec, bad_parameter,
                        "resource_manager::init_allocation",
                        "manage_executor pointer is a nullptr");
                return std::size_t(-1);
            }

            // ask executor for its policies
            error_code ec1(lightweight);
            std::size_t min_punits = proxy->get_policy_element(detail::min_concurrency, ec1);
            if (ec1) min_punits = 1;
            std::size_t max_punits = proxy->get_policy_element(detail::max_concurrency, ec1);
            if (ec1) max_punits = get_os_thread_count();

            // lock the resource manager from this point on
            boost::lock_guard<mutex_type> l(mtx_);

            // allocate initial resources for the given executor
            std::vector<std::pair<std::size_t, std::size_t> > cores =
                allocate_virt_cores(proxy, min_punits, max_punits, ec);
            if (ec) return std::size_t(-1);

            // attach the given proxy to this resource manager
            std::size_t cookie = ++next_cookie_;
            proxies_.insert(proxies_map_type::value_type(
                        cookie, proxy_data(proxy, std::move(cores))));

            if (&ec != &throws)
                ec = make_success_code();
            return cookie;
        }

        void resource_manager::create_dynamicrm_worker()
        {
            {
                resource_manager& rm = get();

                hpx::util::interval_timer timer(
                        &rm->dynamic_resource_manager(), boost::chrono::milliseconds(100)
                        );

                timer.start();

                // wait for timer to have invoked the function 10 times
                while (!timer.is_terminated())
                    hpx::this_thread::yield();
            }

        }

        void resource_manager::do_core_migration()
        {
            preprocess_dynamic_allocation()

                // Exclusive cores are cores that other schedulers can give up (not-shared) or cores that are unused by any scheduler.
                std::size_t exclusive_cores_available = 0;
            // Used cores are cores that are assigned to other schedulers, but are up for grabs, because hill climbing or idle core
            // information has indicated to us that those schedulers can do without them.
            std::size_t used_cores_available = 0;
            std::size_t num_givers = 0;

            // Find schedulers that are able to give up cores.

            std::map<std::size_t, dynamic_allocation_data>::iterator it;
            for (it == proxies_dynamic_allocation_data.begin();it!=proxies_dynamic_allocation_data.end();it++)
            {
                dynamic_allocation_data dt = (*it).second;
                // For all priorities, get the schedulers that we can take cores away from.
                if (dt->GetNumAllocatedCores() > dt.m_suggestedAllocation)
                {
                    // Borrowed cores can be migrated as well. Clearly if the owning scheduler was using the borrowed core, the scheduler
                    // would not still have it. Therefore, the owning scheduler is idle on the core, and if a borrowed core is migrated
                    // the receiver also marks it as 'borrowed'. This also means that the same core can be migrated twice - if two schedulers
                    // have borrowed that core.
                    m_ppGivingProxies[numGivers++] = pDynamicData;
                    usedCoresAvailable += pDynamicData->m_pProxy->GetNumAllocatedCores() - pDynamicData->m_suggestedAllocation;

                    // Find out how many borrowed cores and owned cores this scheduler should give. We first prefer to transfer
                    // borrowed cores before transferring owned cores. Note that all borrowed idle cores should be migrated.
                    pDynamicData->m_borrowedIdleCoresToMigrate = min(pDynamicData->m_numBorrowedIdleCores,
                            pDynamicData->m_pProxy->GetNumAllocatedCores() - pDynamicData->m_suggestedAllocation);

                    pDynamicData->m_borrowedInUseCoresToMigrate = min(pDynamicData->m_pProxy->GetNumBorrowedCores() - pDynamicData->m_numBorrowedIdleCores,
                            pDynamicData->m_pProxy->GetNumAllocatedCores() - pDynamicData->m_suggestedAllocation -
                            pDynamicData->m_borrowedIdleCoresToMigrate);

                    pDynamicData->m_ownedCoresToMigrate = pDynamicData->m_pProxy->GetNumAllocatedCores() - pDynamicData->m_suggestedAllocation -
                        pDynamicData->m_borrowedIdleCoresToMigrate - pDynamicData->m_borrowedInUseCoresToMigrate;
                }
            }

            // Find available cores (cores not assigned to any scheduler), and mark them as reserved.
            std::size_t unused_cores_available = 0;

            // Find cores that are idle, i.e, all schedulers that have that core assigned are not using them at present.
            // We are able to temporarily share these cores with schedulers that indicate that they need cores.
            dynamic_idle_cores_available = 0;

            for (std::size_t i = 0; i != punits_.size(); ++i)
            {
                if (punits_[i].use_count_ == 0)
                {
                    available_punits[i] = punit_status::available;
                    ++unused_cores_available;
                }
                else if (punits_[i].use_count_ == punits_[i].use_count_ )
                {


                }
            }


            else if (pGlobalCore->m_useCount == pGlobalCore->m_idleSchedulers)
            {
                pGlobalCore->m_coreState = ProcessorCore::Idle;
                ++pGlobalNode->m_idleCores;
                // Calculate the total number of idle cores up front. This number could change as we transfer cores between schedulers,
                // and will be updated as we go along.
                ++m_dynamicIdleCoresAvailable;
            }


            exclusive_cores_available = used_cores_available + unused_cores_available;

            // Perform two rounds of allocation/migration.
            // Round 1 : Only consider receivers whose suggested allocation (as given by hill climbing) is higher than their allocated
            // number of cores. After we have exhauted all such receivers, find fully loaded schedulers, and raise their suggested allocation to
            // their desired.
            // Round 2 : If cores are still available do a second round of migration to the new receivers if any.

            for (std::size_t allocation_round = 0; (exclusive_cores_available > 0 || dynamicIdleCoresAvailable > 0) && allocation_round < 2; ++allocation_round)
            {
                if (allocation_round == 1)
                {
                    // This is the second round of allocation. We have already satisfied the increases that hill climbing recommended.
                    // Now we try to find other schedulers who may benefit from resources - since we have some available to give.
                    IncreaseFullyLoadedSchedulerAllocations();
                }

                std::size_t num_receivers = 0;
                std::size_t cores_needed = 0;

                for (it == proxies_dynamic_allocation_data.begin();it!=proxies_dynamic_allocation_data.end();it++)
                {
                    dynamic_allocation_data dt = (*it).second;

                    // Check if there are schedulers that we need to give resources to.
                    DynamicAllocationData * pDynamicData = static_cast<DynamicAllocationData *>(m_ppProxyData[index]);
                    if (pDynamicData->m_pProxy->GetNumAllocatedCores() < pDynamicData->m_suggestedAllocation)
                    {
                        m_ppReceivingProxies[numReceivers++] = pDynamicData;
                        coresNeeded += pDynamicData->m_suggestedAllocation - pDynamicData->m_pProxy->GetNumAllocatedCores();
                    }
                }

                if (num_receivers > 0)
                {
                    // First check for unused cores and cores we can steal from other schedulers. We differentiate between exclusive cores
                    // and idle cores because we first want to satisfy requests using either unused cores or cores other schedulers can give up.
                    if (exclusive_cores_available > 0)
                    {
                        // AdjustDynamicAllocation populates the 'allocation' field of the dynamic data that represents the additional cores we
                        // must give the scheduler. It is guaranteed that we can satisfy all allocations since they will be reduced if the
                        // sum of requested allocations was greater than what was available.
                        std::size_t coresToTransfer = AdjustDynamicAllocation(exclusiveCoresAvailable, coresNeeded, numReceivers);
                        // Find the number of receivers that will still be granted cores (the AdjustDynamicAllocation API above could've reduced
                        // suggested allocations for some receivers), and sort the receivers by number of partially filled nodes.
                        std::size_t exclusiveCoreReceivers = PrepareReceiversForCoreTransfer(numReceivers);

                        // 'coresTransferred' is the total number of cores we are about to distribute among the receivers in the receiving proxy
                        // array. The order in which we give cores is important. We must first give receivers unused cores, then cores taken from
                        // other schedulers, and finally, idle cores.

                        std::size_t unusedCoreQuota = 0;
                        std::size_t usedCoreQuota = 0;
                        std::size_t coresDistributed = 0;

                        coresDistributed = unusedCoreQuota = min(unusedCoresAvailable, coresToTransfer);

                        unusedCoresAvailable -= unusedCoreQuota;

                        if (coresDistributed < coresToTransfer)
                        {
                            unsigned int remainingCores = coresToTransfer - coresDistributed;

                            usedCoreQuota = min(remainingCores, usedCoresAvailable);
                            coresDistributed += usedCoreQuota;
                            usedCoresAvailable -= usedCoreQuota;
                        }

                        DistributeExclusiveCores(coresToTransfer, unusedCoreQuota, usedCoreQuota, exclusiveCoreReceivers, numGivers);

                        exclusiveCoresAvailable -= coresToTransfer;

                        cores_needed -= coresToTransfer;
                    } // end of if (exclusiveCoresAvailable > 0)

                    // Now check if any more requests need to be satisfied. The reason we do this in two stages, (first unused and stolen
                    // cores, followed by idle cores), is that we want to distribute idle cores evenly, since we're temporarily oversubscribing them, and
                    // they could easy be taken away at the next iteration, if the schedulers that were not using the cores start using them.

                    if (cores_needed > 0 && m_dynamicIdleCoresAvailable > 0)
                    {

                        // AdjustDynamicAllocation populates the 'allocation' field of the dynamic data that represents the additional cores we
                        // must give the scheduler. It is guaranteed that we can satisfy all allocations since they will be reduced if the
                        // sum of requested allocations was greater than what was available.
                        unsigned int coresToTransfer = AdjustDynamicAllocation(m_dynamicIdleCoresAvailable, coresNeeded, numReceivers);

                        // Find the number of receivers that will still be granted cores (the AdjustDynamicAllocation API above could've reduced
                        // suggested allocations for some receivers), and sort the receivers by number of partially filled nodes.
                        unsigned int idleCoreReceivers = PrepareReceiversForCoreTransfer(numReceivers);

                        DistributeIdleCores(coresToTransfer, idleCoreReceivers);

                        m_dynamicIdleCoresAvailable -= coresToTransfer;
                        coresNeeded -= coresToTransfer;
                    } // end of if (coresNeeded > 0 && m_dynamicIdleCoresAvailable > 0)
                } // end of if (numReceivers > 0)
            }

        }

        boost::uint64_t get_tick_count()
        {
            return hpx::util::high_resolution_clock::now() / 1000;
        }

        bool resource_manager::dynamic_resource_manager()
        {
            const boost::uint64_t dynamicrm_time_interval = 100; 
            // default time = 100 ms
            boost::uint64_t timeout = dynamicrm_time_interval;
            boost::uint64_t old_tick_count, new_tick_count;
            // simulate a long wait
            old_tick_count = get_tick_count() - (boost::uint64_t)500;
            new_tick_count = 0;

            while(dynamicrmworker_state != exit_thread)
            {
                boost::uint64_t retval = wait_for_event();
                {
                    switch (dynamicrmworker_state)
                    {
                        case standby:
                            {
                                // We're holding the lock, and the state is Standby. There should be only one
                                // scheduler the RM knows about at this time.
                                if (DistributeCoresToSurvivingScheduler())
                                {
                                    timeout = INFINITE;
                                }
                                else
                                {
                                    // We might fail distributing cores to a scheduler if
                                    // it has yet to be retired vprocs on cores that were
                                    // removed previously. Since there is no DRM, we need
                                    // to retry until the scheduler has the desired number
                                    // of hardware threads.
                                    timeout = dynamicrm_time_interval;
                                }
                                break;
                                return false;
                            }

                        case load_balance:
                            {
                                if (retval == )
                                {
                                    DoCoreMigration();
                                    if (SchedulersNeedNotifications())
                                    {
                                        SendResourceNotifications();
                                    }

                                    old_tick_count = get_tick_count();
                                    timeout = dynamicrm_time_interval;
                                }
                                else
                                {
                                    new_tick_count = get_tick_count();
                                    boost::uint64_t tick_difference = (boost::uint64_t) ((new_tick_count - old_tick_count)/1000.0);
                                    if (tick_difference > dynamicrm_time_interval)
                                    {
                                        // We're holding the lock, and the state is LoadBalance. There should be at least two
                                        // schedulers the RM knows about at this time.
                                        if (tick_difference > dynamicrm_time_interval + 30)
                                        {
                                            // Since GetTickCount is accurate upto 10-15ms, do not throw away statistics,
                                            // unless we've waited for a 'long' time.
                                            DiscardExistingSchedulerStatistics();
                                        }
                                        else if (SchedulersNeedNotifications())
                                        {
                                            SendResourceNotifications();
                                        }

                                        old_tick_count = get_tick_count();
                                        timeout = dynamicrm_time_interval;
                                    }
                                    else
                                    {
                                        // We were woken up within the 100 ms interval - most likely so that we could send notifications.
                                        if (SchedulersNeedNotifications())
                                        {
                                            SendResourceNotifications();
                                        }
                                        timeout = dynamicrm_time_interval - tick_difference;
                                    }

                                    return true;
                                }
                                break;
                            }
                        case exit_thread:
                        default:
                            {
                                // We are shutting down
                                break;
                                return false;
                            }

                    }; // end of switch 
                } // end locked region 
            } // end while
        }

        /// <summary>
        ///     When the number of schedulers in the RM goes from 2 to 1, this routine is invoked to make sure the remaining scheduler
        ///     has its desired number of cores, before putting the dynamic RM worker on standby. It is also called when there is just
        ///     one scheduler with external subscribed threads that it removes -> there is a chance that this move may allow us to allocate
        ///     more vprocs.
        /// </summary>
        bool ResourceManager::DistributeCoresToSurvivingScheduler()
        {
            // NOTE: This routine must be called while m_lock is held.

            if (!m_schedulers.Empty())
            {
                SchedulerProxy * pSchedulerProxy = m_schedulers.First();

                // Since this is the only scheduler in the RM, we should able to satisfy its MaxConcurrency.
                if (pSchedulerProxy->GetNumAllocatedCores() < pSchedulerProxy->DesiredHWThreads() ||
                        pSchedulerProxy->GetNumBorrowedCores() > 0)
                {
                    unsigned int suggestedAllocation = pSchedulerProxy->AdjustAllocationIncrease(pSchedulerProxy->DesiredHWThreads());
                    unsigned int remainingCores = suggestedAllocation - pSchedulerProxy->GetNumAllocatedCores();
                    SchedulerNode * pAllocatedNodes = pSchedulerProxy->GetAllocatedNodes();
                    unsigned int * pSortedNodeOrder = pSchedulerProxy->GetSortedNodeOrder();

                    // Sort the array of nodes in the proxy by number of allocated cores, largest first, if we're allocating
                    // to it less cores than the total available. This is so that we pack nodes as tightly as possible.
                    bool sortNodes = pSchedulerProxy->DesiredHWThreads() != m_coreCount;

                    for (unsigned int i = 0; i < m_nodeCount; ++i)
                    {
                        // No need to sort nodes the next time around, if there are no more cores to add.
                        sortNodes &= remainingCores > 0;

                        if (sortNodes)
                        {
                            unsigned int maxAllocationIndex = i;
                            SchedulerNode *pMaxNode = &pAllocatedNodes[pSortedNodeOrder[maxAllocationIndex]];

                            for (unsigned int j = i + 1; j < m_nodeCount; ++j)
                            {
                                SchedulerNode * pNode = &pAllocatedNodes[pSortedNodeOrder[j]];
                                if (pNode->m_allocatedCores > pMaxNode->m_allocatedCores)
                                {
                                    maxAllocationIndex = j;
                                    pMaxNode = pNode;
                                }
                            }

                            if (i != maxAllocationIndex)
                            {
                                // Swap the index at 'maxAllocationIndex' with the index at 'i'. The next iteration will traverse nodes starting at
                                // m_pSortedNodeOrder[i + i].
                                unsigned int tempIndex = pSortedNodeOrder[i];
                                pSortedNodeOrder[i] = pSortedNodeOrder[maxAllocationIndex];
                                pSortedNodeOrder[maxAllocationIndex] = tempIndex;
                            }
                        }

                        // Assign cores until the desired number of cores is reached. In addition, check if there are
                        // any borrowed cores and switch them to owned.
                        SchedulerNode * pCurrentNode = &pAllocatedNodes[pSortedNodeOrder[i]];
                        for (unsigned int coreIndex = 0; coreIndex < pCurrentNode->m_coreCount; ++coreIndex)
                        {
                            SchedulerCore * pCore = &pCurrentNode->m_pCores[coreIndex];
                            if (pCore->m_coreState == ProcessorCore::Unassigned)
                            {
                                if (remainingCores > 0)
                                {
                                    ASSERT(*pCore->m_pGlobalUseCountPtr == 0);

                                    ++(*pCore->m_pGlobalUseCountPtr);

                                    pSchedulerProxy->AddCore(pCurrentNode, coreIndex, false);
                                    --remainingCores;
                                }
                            }
                            else
                            {
                                if (pCore->IsBorrowed())
                                {
                                    pSchedulerProxy->ToggleBorrowedState(pCurrentNode, coreIndex);
                                }
                            }
                        }
                    }
                }

                if (pSchedulerProxy->ShouldReceiveNotifications())
                {
                    SendResourceNotifications();
                }

                return (pSchedulerProxy->GetNumAllocatedCores() == pSchedulerProxy->DesiredHWThreads());
            }

            return true;
        }


        // Find 'desired' amount of processing units which have the given use count
        // (use count is the number of schedulers associated with a given processing
        // unit).
        //
        // the resource manager is locked while executing this function
        std::size_t resource_manager::reserve_processing_units(
                std::size_t use_count, std::size_t desired,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            std::size_t available = 0;
            for (std::size_t i = 0; i != punits_.size(); ++i)
            {
                if (use_count == punits_[i].use_count_)
                {
                    available_punits[i] = punit_status::reserved;
                    if (++available == desired)
                        break;
                }
            }
            return available;
        }

        std::size_t resource_manager::reserve_at_higher_use_count(
                std::size_t desired,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            std::size_t use_count = 1;
            std::size_t available = 0;

            while (available < desired)
            {
                available += reserve_processing_units(
                        use_count++, desired - available, available_punits);
            }

            return available;
        }


        // Instructs a scheduler proxy to free up a fixed number of resources
        // This is only a temporary release of resources.
        // The use count on the global core is decremented and the scheduler
        // proxy remembers the core as temporarily released
        // release_cores_to_min - scheduler should release all cores above its minimum
        // release_borrowed_cores - scheduler should release all its borrowed cores

        bool resource_manager::release_scheduler_resources(std::map<std::size_t, static_allocation_data>::iterator it, 
                std::size_t number_to_free, std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            static_allocation_data st;
            std::size_t borrowed_cores;
            std::size_t owned_cores;

            st = (*it).second;

            proxies_map_type::iterator iter;
            iter = proxies_.find((*it).first);
            proxy_data& p = (*iter).second;

            if (number_to_free == release_borrowed_cores)
            {
                number_to_free = borrowed_cores = st.num_borrowed_cores;
            }

            else if (number_to_free == release_cores_to_min)
            {
                number_to_free = st.num_owned_cores - st.min_proxy_cores;
                borrowed_cores = 0;
            }

            else
            {
                borrowed_cores = 0;
            }

            owned_cores = number_to_free - borrowed_cores;

            if (number_to_free > 0)
            {
                for (coreids_type coreids : p.core_ids_) {
                    if (punits_[coreids.first].use_count_ > 0 || owned_cores > 0)
                    {
                        // The proxy remembers this processor as gone ..
                        // TODO

                        --punits_[coreids.first].use_count_;

                        if (punits_[coreids.first].use_count_ > 0)
                        {
                            --owned_cores;
                        }


                        if (--number_to_free == 0)
                        {
                            return true;
                        }
                    }
                }
            }

            // The scheduler proxy does not have any cores available to free.
            return false;

        }


        // Instructs existing schedulers to release cores. Then tries to reserve
        // available cores for the new scheduler

        std::size_t resource_manager::release_cores_on_existing_scedulers(std::size_t number_to_free,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            // Ask previously allocated schedulers to release surplus cores, until either the request is satisfied, or we're out of schedulers.
            bool released_cores = false;

            std::map<std::size_t, static_allocation_data>::iterator it;

            for (it = proxies_static_allocation_data.begin();it!=proxies_static_allocation_data.end();it++)
            {
                if (release_scheduler_resources(it, number_to_free, available_punits))
                {
                    released_cores = true;
                }

            }

            std::size_t available;

            if (released_cores)
            {
                available = reserve_processing_units(0, number_to_free, available_punits);
            }
            else
            {
                available = 0;
            }

            return available;
        }

        // Tries to redistribute cores allocated to all schedulers proportional 
        // to each schedulers maximum punits 
        // and reserve any freed cores for the new scheduler.
        std::size_t resource_manager::redistribute_cores_among_all(std::size_t reserved, std::size_t min_punits, std::size_t max_punits,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {

            std::size_t available = 0;

            // Try to proportionally allocate cores to all schedulers w/o oversubscription. The proportions used will be
            // max_punits for each scheduler, except that no existing scheduler will be forced to increase the current allocation.
            if (proxies_static_allocation_data.size() > 1)
            {
                std::size_t total_minimum = min_punits;
                std::size_t total_allocated = reserved; // sum of cores that have been previously reserved and cores that were reserved during this allocation attempt.
                std::size_t num_schedulers = 1; // includes the current scheduler

                // total_allocated isnumber of cores allocated to new scheduler so far plus 
                // the number of 'owned' cores allocated to all existing schedulers.

                std::map<std::size_t, static_allocation_data>::iterator it;
                for (it == proxies_static_allocation_data.begin();it!=proxies_static_allocation_data.end();it++)
                {

                    static_allocation_data st = (*it).second;

                    if (st.num_owned_cores > st.min_proxy_cores)
                    {
                        ++num_schedulers;
                        total_minimum += st.min_proxy_cores;
                        total_allocated += st.num_owned_cores;
                    }
                }

                if (num_schedulers > 1 && total_minimum <= total_allocated)
                {
                    // We have found schedulers with cores greater than min. Moreover, the sum of all cores already allocated to
                    // existing schedulers can at least satisfy all mins (including the min requirement of the current scheduler).
                    std::size_t cookie = next_cookie_ + 1;

                    double total_desired = 0.0;
                    double scaling = 0.0;

                    std::map<std::size_t, static_allocation_data> scaled_static_allocation_data;	

                    static_allocation_data st;
                    st.min_proxy_cores = min_punits;
                    st.max_proxy_cores = max_punits;	
                    st.adjusted_desired = max_punits;

                    total_desired += st.adjusted_desired;

                    scaled_static_allocation_data.insert(std::map<std::size_t, static_allocation_data>::value_type(cookie , st)
