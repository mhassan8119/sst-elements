#include <sst_config.h>

#include "arielcore.h"

ArielCore::ArielCore(int fd_in, SimpleMem* coreToCacheLink, uint32_t thisCoreID,
	uint32_t maxPendTrans, Output* out, uint32_t maxIssuePerCyc,
	uint32_t maxQLen, int pipeTO, uint64_t cacheLineSz, SST::Component* own,
			ArielMemoryManager* memMgr, const uint32_t perform_address_checks, const std::string traceFilePrefix) :
		perform_checks(perform_address_checks),
		enableTracing((traceFilePrefix != "")) {

	output = out;
	verbosity = (uint32_t) output->getVerboseLevel();
	output->verbose(CALL_INFO, 2, 0, "Creating core with ID %" PRIu32 ", maximum queue length=%" PRIu32 ", max issue is: %" PRIu32 "\n", thisCoreID, maxQLen, maxIssuePerCyc);
	fd_input = fd_in;
	cacheLink = coreToCacheLink;
	coreID = thisCoreID;
	maxPendingTransactions = maxPendTrans;
	isHalted = false;
	maxIssuePerCycle = maxIssuePerCyc;
	maxQLength = maxQLen;
	readPipeTimeOut = pipeTO;
	cacheLineSize = cacheLineSz;
	owner = own;
	memmgr = memMgr;

	coreQ = new std::queue<ArielEvent*>();
	pendingTransactions = new std::map<SimpleMem::Request::id_t, SimpleMem::Request*>();
	pending_transaction_count = 0;

	read_requests = 0;
	write_requests = 0;
	split_read_requests = 0;
	split_write_requests = 0;
	noop_count = 0;

	if(0 == thisCoreID) {
		output->verbose(CALL_INFO, 1, 0, "Waiting for core 0 to poll for initial data (means application will have launched and attached)\n");

		struct pollfd poll_input;
	        poll_input.fd = fd_in;
       		poll_input.events = POLLIN;

		// Configure for an infinite timeout waiting for core 0 to begin.
		int poll_result = poll(&poll_input, (unsigned int) 1, (int) -1);

		if(poll_result == -1) {
                        output->fatal(CALL_INFO, -2, "Attempt to poll failed.\n");
                }
	}

	// If we enabled tracing then open up the correct file.
	if(enableTracing) {
		char* traceFilePath = (char*) malloc( sizeof(char) * (traceFilePrefix.size() + 20) );
		sprintf(traceFilePath, "%s-%d.trace", traceFilePrefix.c_str(), (int) thisCoreID);
		traceFile = fopen(traceFilePath, "wt");
		free(traceFilePath);
	}

	// Get a time converter from the core, we want nano seconds.
	picoTimeConv = Simulation::getSimulation()->getTimeLord()->getTimeConverter("1ps");
}

ArielCore::~ArielCore() {

}

void ArielCore::setCacheLink(SimpleMem* newLink) {
	cacheLink = newLink;
}

void ArielCore::printTraceEntry(const bool isRead,
                       	const uint64_t address, const uint32_t length) {

	if(enableTracing) {
		uint64_t picoSeconds = (uint64_t) picoTimeConv->convertFromCoreTime(Simulation::getSimulation()->getCurrentSimCycle());

		fprintf(traceFile, "%" PRIu64 " %c %" PRIu64 " %" PRIu32 "\n",
			(uint64_t) picoSeconds, (isRead ? 'R' : 'W'), address, length);
	}
}

void ArielCore::commitReadEvent(const uint64_t address, const uint32_t length) {
	if(length > 0) {
        SimpleMem::Request *req = new SimpleMem::Request(SimpleMem::Request::Read, address, length);

		pending_transaction_count++;
	        pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );

	        if(enableTracing) {
	        	printTraceEntry(true, (const uint64_t) req->addr, (const uint32_t) length);
	        }

	        // Actually send the event to the cache
	        cacheLink->sendRequest(req);
	}
}

void ArielCore::commitWriteEvent(const uint64_t address, const uint32_t length) {
	if(length > 0) {
        SimpleMem::Request *req = new SimpleMem::Request(SimpleMem::Request::Write, address, length);
        // TODO BJM:  DO we need to fill in dummy data?

		pending_transaction_count++;
	        pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );

	        if(enableTracing) {
	        	printTraceEntry(false, (const uint64_t) req->addr, (const uint32_t) length);
	        }

	        // Actually send the event to the cache
        	cacheLink->sendRequest(req);
	}
}

void ArielCore::handleEvent(SimpleMem::Request* event) {
    output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " handling a memory event.\n", coreID);

    SimpleMem::Request::id_t mev_id = event->id;
    std::map<SimpleMem::Request::id_t, SimpleMem::Request*>::iterator find_entry = pendingTransactions->find(mev_id);

    if(find_entry != pendingTransactions->end()) {
        output->verbose(CALL_INFO, 4, 0, "Correctly identified event in pending transactions, removing from list leaving: %" PRIu32 " transactions\n",
                (uint32_t) pendingTransactions->size());

        pendingTransactions->erase(find_entry);
        pending_transaction_count--;
    } else {
        output->fatal(CALL_INFO, -4, "Memory event response to core: %" PRIu32 " was not found in pending list.\n", coreID);
    }
    delete event;
}

void ArielCore::finishCore() {
	// Close the trace file if we did in fact open it.
	if(enableTracing) {
		fclose(traceFile);
	}
}

void ArielCore::halt() {
	isHalted = true;
}

void ArielCore::closeInput() {
	close(fd_input);
}

void ArielCore::handleSwitchPoolEvent(ArielSwitchPoolEvent* aSPE) {
	output->verbose(CALL_INFO, 2, 0, "Core: %" PRIu32 " set default memory pool to: %" PRIu32 "\n", coreID, aSPE->getPool());
	memmgr->setDefaultPool(aSPE->getPool());
}

void ArielCore::createSwitchPoolEvent(uint32_t newPool) {
	ArielSwitchPoolEvent* ev = new ArielSwitchPoolEvent(newPool);
	coreQ->push(ev);

	output->verbose(CALL_INFO, 4, 0, "Generated a switch pool event on core %" PRIu32 ", new level is: %" PRIu32 "\n", coreID, newPool);
}

void ArielCore::createNoOpEvent() {
	ArielNoOpEvent* ev = new ArielNoOpEvent();
	coreQ->push(ev);

	output->verbose(CALL_INFO, 4, 0, "Generated a No Op event on core %" PRIu32 "\n", coreID);
}

void ArielCore::createReadEvent(uint64_t address, uint32_t length) {
	ArielReadEvent* ev = new ArielReadEvent(address, length);
	coreQ->push(ev);

	output->verbose(CALL_INFO, 4, 0, "Generated a READ event, addr=%" PRIu64 ", length=%" PRIu32 "\n", address, length);
}

void ArielCore::createAllocateEvent(uint64_t vAddr, uint64_t length, uint32_t level) {
	ArielAllocateEvent* ev = new ArielAllocateEvent(vAddr, length, level);
	coreQ->push(ev);

	output->verbose(CALL_INFO, 2, 0, "Generated an allocate event, vAddr(map)=%" PRIu64 ", length=%" PRIu64 " in level %" PRIu32 "\n",
		vAddr, length, level);
}

void ArielCore::createFreeEvent(uint64_t vAddr) {
	ArielFreeEvent* ev = new ArielFreeEvent(vAddr);
	coreQ->push(ev);

	output->verbose(CALL_INFO, 2, 0, "Generated a free event for virtual address=%" PRIu64 "\n", vAddr);
}

void ArielCore::createWriteEvent(uint64_t address, uint32_t length) {
	ArielWriteEvent* ev = new ArielWriteEvent(address, length);
	coreQ->push(ev);
	
	output->verbose(CALL_INFO, 4, 0, "Generated a WRITE event, addr=%" PRIu64 ", length=%" PRIu32 "\n", address, length);
}

void ArielCore::createExitEvent() {
	ArielExitEvent* xEv = new ArielExitEvent();
	coreQ->push(xEv);
	
	output->verbose(CALL_INFO, 4, 0, "Generated an EXIT event.\n");
}

bool ArielCore::isCoreHalted() {
	return isHalted;
}

bool ArielCore::refillQueue() {
	output->verbose(CALL_INFO, 16, 0, "Refilling event queue for core %" PRIu32 "...\n", coreID);

	struct pollfd poll_input;
	poll_input.fd = fd_input;
	poll_input.events = POLLIN;
	bool added_data = false;

	while(coreQ->size() < maxQLength) {
		output->verbose(CALL_INFO, 16, 0, "Attempting to fill events for core: %" PRIu32 " current queue size=%" PRIu32 ", max length=%" PRIu32 "\n",
			coreID, (uint32_t) coreQ->size(), (uint32_t) maxQLength);

		int poll_result = poll(&poll_input, (unsigned int) 1, (int) readPipeTimeOut);
		if(poll_result == -1) {
			output->fatal(CALL_INFO, -2, "Attempt to poll failed.\n");
			break;
		}

		if(poll_input.revents & POLLIN) {
			output->verbose(CALL_INFO, 32, 0, "Pipe poll reads data on core: %" PRIu32 "\n", coreID);
			// There is data on the pipe
			added_data = true;

			uint8_t command = 0;
			read(fd_input, &command, sizeof(command));

			switch(command) {
			case ARIEL_START_INSTRUCTION:
				uint64_t op_addr;
				uint32_t op_size;

				while(command != ARIEL_END_INSTRUCTION) {
					read(fd_input, &command, sizeof(command));

					switch(command) {
					case ARIEL_PERFORM_READ:
						read(fd_input, &op_addr, sizeof(op_addr));
						read(fd_input, &op_size, sizeof(op_size));

						createReadEvent(op_addr, op_size);
						break;

					case ARIEL_PERFORM_WRITE:
						read(fd_input, &op_addr, sizeof(op_addr));
						read(fd_input, &op_size, sizeof(op_size));

						createWriteEvent(op_addr, op_size);
						break;

					case ARIEL_END_INSTRUCTION:
						break;

					default:
						// Not sure what this is
						assert(0);
						break;
					}
				}
				break;

			case ARIEL_NOOP:
				createNoOpEvent();
				break;

			case ARIEL_ISSUE_TLM_MAP:
				uint64_t tlm_map_vaddr;
				uint64_t tlm_alloc_len;
				uint32_t tlm_alloc_level;

				read(fd_input, &tlm_map_vaddr, sizeof(tlm_map_vaddr));
				read(fd_input, &tlm_alloc_len, sizeof(tlm_alloc_len));
				read(fd_input, &tlm_alloc_level, sizeof(tlm_alloc_level));

				createAllocateEvent(tlm_map_vaddr, tlm_alloc_len, tlm_alloc_level);

				break;

			case ARIEL_ISSUE_TLM_FREE:
				uint64_t tlm_free_vaddr;

				read(fd_input, &tlm_free_vaddr, sizeof(tlm_free_vaddr));

				createFreeEvent(tlm_free_vaddr);

				break;

			case ARIEL_SWITCH_POOL:
				uint32_t new_pool;

				read(fd_input, &new_pool, sizeof(new_pool));
				createSwitchPoolEvent(new_pool);

				break;

			case ARIEL_PERFORM_EXIT:
				createExitEvent();
				break;
			}
		} else {
			return added_data;
		}
	}

	output->verbose(CALL_INFO, 16, 0, "Refilling event queue for core %" PRIu32 " is complete, added data? %s\n", coreID,
		(added_data ? "yes" : "no"));
	return added_data;
}

void ArielCore::handleFreeEvent(ArielFreeEvent* rFE) {
	output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " processing a free event (for virtual address=%" PRIu64 ")\n", coreID, rFE->getVirtualAddress());

	memmgr->free(rFE->getVirtualAddress());
}

void ArielCore::handleReadRequest(ArielReadEvent* rEv) {
	output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " processing a read event...\n", coreID);

	const uint64_t readAddress = rEv->getAddress();
	const uint64_t readLength  = (uint64_t) rEv->getLength();

	if(readLength > cacheLineSize) {
		output->verbose(CALL_INFO, 4, 0, "Potential error? request for a read of length=%" PRIu64 " is larger than cache line which is not allowed (coreID=%" PRIu32 ", cache line: %" PRIu64 "\n",
			readLength, coreID, cacheLineSize);
		return;
	}

	const uint64_t addr_offset  = readAddress % ((uint64_t) cacheLineSize);

	if((addr_offset + readLength) <= cacheLineSize) {
		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " generating a non-split read request: Addr=%" PRIu64 " Length=%" PRIu64 "\n",
			coreID, readAddress, readLength);

		// We do not need to perform a split operation
		const uint64_t physAddr = memmgr->translateAddress(readAddress);

		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " issuing read, VAddr=%" PRIu64 ", Size=%" PRIu64 ", PhysAddr=%" PRIu64 "\n", 
			coreID, readAddress, readLength, physAddr);

		commitReadEvent(physAddr, (uint32_t) readLength);
	} else {
		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " generating a split read request: Addr=%" PRIu64 " Length=%" PRIu64 "\n",
			coreID, readAddress, readLength);

		// We need to perform a split operation
		const uint64_t leftAddr = readAddress;
		const uint64_t leftSize = cacheLineSize - addr_offset;

		const uint64_t rightAddr = (readAddress - addr_offset) + ((uint64_t) cacheLineSize);
		const uint64_t rightSize = (readAddress + ((uint64_t) readLength)) % ((uint64_t) cacheLineSize);

		const uint64_t physLeftAddr = memmgr->translateAddress(leftAddr);
		const uint64_t physRightAddr = memmgr->translateAddress(rightAddr);

		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " issuing split-address read, LeftVAddr=%" PRIu64 ", RightVAddr=%" PRIu64 ", LeftSize=%" PRIu64 ", RightSize=%" PRIu64 ", LeftPhysAddr=%" PRIu64 ", RightPhysAddr=%" PRIu64 "\n", 
			coreID, leftAddr, rightAddr, leftSize, rightSize, physLeftAddr, physRightAddr);

		if(perform_checks > 0) {
			if( (leftSize + rightSize) != readLength ) {
				output->fatal(CALL_INFO, -4, "Core %" PRIu32 " read request for address %" PRIu64 ", length=%" PRIu64 ", split into left address=%" PRIu64 ", left size=%" PRIu64 ", right address=%" PRIu64 ", right size=%" PRIu64 " does not equal read length (cache line of length %" PRIu64 ")\n",
					coreID, readAddress, readLength, leftAddr, leftSize, rightAddr, rightSize, cacheLineSize);
			}

			if( ((leftAddr + leftSize) % cacheLineSize) != 0) {
				output->fatal(CALL_INFO, -4, "Error leftAddr=%" PRIu64 " + size=%" PRIu64 " is not a multiple of cache line size: %" PRIu64 "\n",
					leftAddr, leftSize, cacheLineSize);
			}

			if( ((rightAddr + rightSize) % cacheLineSize) > cacheLineSize ) {
				output->fatal(CALL_INFO, -4, "Error rightAddr=%" PRIu64 " + size=%" PRIu64 " is not a multiple of cache line size: %" PRIu64 "\n",
					leftAddr, leftSize, cacheLineSize);
			}
		}

		commitReadEvent(physLeftAddr, (uint32_t) leftSize);
		commitReadEvent(physRightAddr, (uint32_t) rightSize);
		split_read_requests++;
	}

	read_requests++;
}

void ArielCore::handleWriteRequest(ArielWriteEvent* wEv) {
	output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " processing a write event...\n", coreID);
	
	const uint64_t writeAddress = wEv->getAddress();
	const uint64_t writeLength  = wEv->getLength();
	
	if(writeLength > cacheLineSize) {
		output->verbose(CALL_INFO, 4, 0, "Potential error? request for a write of length=%" PRIu64 " is larger than cache line which is not allowed (coreID=%" PRIu32 ", cache line: %" PRIu64 "\n",
			writeLength, coreID, cacheLineSize);
		return;
	}
	
	const uint64_t addr_offset  = writeAddress % ((uint64_t) cacheLineSize);
	
	if((addr_offset + writeLength) <= cacheLineSize) {
		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " generating a non-split write request: Addr=%" PRIu64 " Length=%" PRIu64 "\n",
			coreID, writeAddress, writeLength);
	
		// We do not need to perform a split operation
		const uint64_t physAddr = memmgr->translateAddress(writeAddress);
		
		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " issuing write, VAddr=%" PRIu64 ", Size=%" PRIu64 ", PhysAddr=%" PRIu64 "\n", 
			coreID, writeAddress, writeLength, physAddr);

		commitWriteEvent(physAddr, (uint32_t) writeLength);
	} else {
		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " generating a split write request: Addr=%" PRIu64 " Length=%" PRIu64 "\n",
			coreID, writeAddress, writeLength);
	
		// We need to perform a split operation
		const uint64_t leftAddr = writeAddress;
		const uint64_t leftSize = cacheLineSize - addr_offset;

		const uint64_t rightAddr = (writeAddress - addr_offset) + ((uint64_t) cacheLineSize);
		const uint64_t rightSize = (writeAddress + ((uint64_t) writeLength)) % ((uint64_t) cacheLineSize);

		const uint64_t physLeftAddr = memmgr->translateAddress(leftAddr);
		const uint64_t physRightAddr = memmgr->translateAddress(rightAddr);

		output->verbose(CALL_INFO, 4, 0, "Core %" PRIu32 " issuing split-address write, LeftVAddr=%" PRIu64 ", RightVAddr=%" PRIu64 ", LeftSize=%" PRIu64 ", RightSize=%" PRIu64 ", LeftPhysAddr=%" PRIu64 ", RightPhysAddr=%" PRIu64 "\n", 
			coreID, leftAddr, rightAddr, leftSize, rightSize, physLeftAddr, physRightAddr);

		if(perform_checks > 0) {
			if( (leftSize + rightSize) != writeLength ) {
				output->fatal(CALL_INFO, -4, "Core %" PRIu32 " write request for address %" PRIu64 ", length=%" PRIu64 ", split into left address=%" PRIu64 ", left size=%" PRIu64 ", right address=%" PRIu64 ", right size=%" PRIu64 " does not equal write length (cache line of length %" PRIu64 ")\n",
					coreID, writeAddress, writeLength, leftAddr, leftSize, rightAddr, rightSize, cacheLineSize);
			}

			if( ((leftAddr + leftSize) % cacheLineSize) != 0) {
				output->fatal(CALL_INFO, -4, "Error leftAddr=%" PRIu64 " + size=%" PRIu64 " is not a multiple of cache line size: %" PRIu64 "\n",
					leftAddr, leftSize, cacheLineSize);
			}

			if( ((rightAddr + rightSize) % cacheLineSize) > cacheLineSize ) {
				output->fatal(CALL_INFO, -4, "Error rightAddr=%" PRIu64 " + size=%" PRIu64 " is not a multiple of cache line size: %" PRIu64 "\n",
					leftAddr, leftSize, cacheLineSize);
			}
		}

		commitWriteEvent(physLeftAddr, (uint32_t) leftSize);
		commitWriteEvent(physRightAddr, (uint32_t) rightSize);
		split_write_requests++;	
	}
	
	write_requests++;
}

void ArielCore::handleAllocationEvent(ArielAllocateEvent* aEv) {
	output->verbose(CALL_INFO, 2, 0, "Handling a memory allocation event, vAddr=%" PRIu64 ", length=%" PRIu64 ", at level=%" PRIu32 "\n",
		aEv->getVirtualAddress(), aEv->getAllocationLength(), aEv->getAllocationLevel());

	memmgr->allocate(aEv->getAllocationLength(), aEv->getAllocationLevel(), aEv->getVirtualAddress());
}

void ArielCore::printCoreStatistics() {
	output->verbose(CALL_INFO, 1, 0, "Core %" PRIu32 " Statistics:\n", coreID);
	output->verbose(CALL_INFO, 1, 0, "- Total Read Requests:         %" PRIu64 "\n", read_requests);
	output->verbose(CALL_INFO, 1, 0, "- Split Read Requests:         %" PRIu64 "\n", split_read_requests);
	output->verbose(CALL_INFO, 1, 0, "- Total Write Requests:        %" PRIu64 "\n", write_requests);
	output->verbose(CALL_INFO, 1, 0, "- Split Write Requests:        %" PRIu64 "\n", split_write_requests);
	output->verbose(CALL_INFO, 1, 0, "- Total Requests:              %" PRIu64 "\n", (read_requests + write_requests));
	output->verbose(CALL_INFO, 1, 0, "- No Mem Op Insts:             %" PRIu64 "\n", noop_count);
}

bool ArielCore::processNextEvent() {
	// Attempt to refill the queue
	if(coreQ->empty()) {
		bool addedItems = refillQueue();

		output->verbose(CALL_INFO, 16, 0, "Attempted a queue fill, %s data\n",
			(addedItems ? "added" : "did not add"));

		if(! addedItems) {
			return false;
		}
	}
	
	output->verbose(CALL_INFO, 8, 0, "Processing next event in core %" PRIu32 "...\n", coreID);

	ArielEvent* nextEvent = coreQ->front();
	bool removeEvent = false;

	switch(nextEvent->getEventType()) {
	case NOOP:
		if(verbosity > 8) output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is NOOP\n", coreID);

		noop_count++;
		removeEvent = true;
		break;

	case READ_ADDRESS:
		if(verbosity > 8) output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is READ_ADDRESS\n", coreID);

//		if(pendingTransactions->size() < maxPendingTransactions) {
		if(pending_transaction_count < maxPendingTransactions) {
			output->verbose(CALL_INFO, 16, 0, "Found a read event, fewer pending transactions than permitted so will process...\n");
			removeEvent = true;
			handleReadRequest(dynamic_cast<ArielReadEvent*>(nextEvent));
		} else {
			output->verbose(CALL_INFO, 16, 0, "Pending transaction queue is currently full for core %" PRIu32 ", core will stall for new events\n", coreID);
			break;
		}
		break;

	case WRITE_ADDRESS:
		if(verbosity > 8) output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is WRITE_ADDRESS\n", coreID);

//		if(pendingTransactions->size() < maxPendingTransactions) {
		if(pending_transaction_count < maxPendingTransactions) {
			output->verbose(CALL_INFO, 16, 0, "Found a write event, fewer pending transactions than permitted so will process...\n");
			removeEvent = true;
			handleWriteRequest(dynamic_cast<ArielWriteEvent*>(nextEvent));
		} else {
			output->verbose(CALL_INFO, 16, 0, "Pending transaction queue is currently full for core %" PRIu32 ", core will stall for new events\n", coreID);
			break;
		}
		break;

	case START_DMA_TRANSFER:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is START_DMA_TRANSFER\n", coreID);
		removeEvent = true;
		break;

	case WAIT_ON_DMA_TRANSFER:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is WAIT_ON_DMA_TRANSFER\n", coreID);
		removeEvent = true;
		break;

	case SWITCH_POOL:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is a SWITCH_POOL\n",
			coreID);
		removeEvent = true;
		handleSwitchPoolEvent(dynamic_cast<ArielSwitchPoolEvent*>(nextEvent));
		break;

	case FREE:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is FREE\n", coreID);
		removeEvent = true;
		handleFreeEvent(dynamic_cast<ArielFreeEvent*>(nextEvent));
		break;

	case MALLOC:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is MALLOC\n", coreID);
		removeEvent = true;
		handleAllocationEvent(dynamic_cast<ArielAllocateEvent*>(nextEvent));
		break;

	case CORE_EXIT:
		output->verbose(CALL_INFO, 8, 0, "Core %" PRIu32 " next event is CORE_EXIT\n", coreID);
		isHalted = true;
		std::cout << "CORE ID: " << coreID << " PROCESSED AN EXIT EVENT" << std::endl;
		output->verbose(CALL_INFO, 2, 0, "Core %" PRIu32 " has called exit.\n", coreID);
		return true;

	default:
		output->fatal(CALL_INFO, -4, "Unknown event type has arrived on core %" PRIu32 "\n", coreID);
		break;
	}

	// If the event has actually been processed this cycle then remove it from the queue
	if(removeEvent) {
		output->verbose(CALL_INFO, 8, 0, "Removing event from pending queue, there are %" PRIu32 " events in the queue before deletion.\n", 
			(uint32_t) coreQ->size());
		coreQ->pop();

		delete nextEvent;
		return true;
	} else {
		output->verbose(CALL_INFO, 8, 0, "Event removal was not requested, pending transaction queue length=%" PRIu32 ", maximum transactions: %" PRIu32 "\n",
			(uint32_t)pendingTransactions->size(), maxPendingTransactions);
		return false;
	}
}

void ArielCore::tick() {
	if(! isHalted) {
		output->verbose(CALL_INFO, 16, 0, "Ticking core id %" PRIu32 "\n", coreID);
		for(uint32_t i = 0; i < maxIssuePerCycle; ++i) {
//			output->verbose(CALL_INFO, 16, 0, "Issuing event %" PRIu32 " out of max issue: %" PRIu32 "...\n", i, maxIssuePerCycle);
			bool didProcess = processNextEvent();

			// If we didnt process anything in the call or we have halted then 
			// we stop the ticking and return
			if( (!didProcess) || isHalted) {
				break;
			}
		}
	}
}
