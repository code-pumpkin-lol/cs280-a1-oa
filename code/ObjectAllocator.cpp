#include "ObjectAllocator.h"

#define PTR_SIZE sizeof(word_t) //! Size of a pointer.
#define INCREMENT_PTR(ptr) (ptr + 1) //! Move the pointer by one
using word_t = intptr_t ;

ObjectAllocator::ObjectAllocator(size_t ObjectSize, 
                                const OAConfig& config):
                                OAConfig_(config)
{
    this->OAStats_.ObjectSize_ = ObjectSize;
    this->OAStats_.PageSize_ = sizeof(*PageList_) +
                                this->OAConfig_.LeftAlignSize_ +
                                this->OAConfig_.ObjectsPerPage_ *ObjectSize -
                                this->OAConfig_.InterAlignSize_;
    this->headerSize = 
        OAConfig_.Alignment_ ? OAConfig_.Alignment_ * (((PTR_SIZE + OAConfig_.HBlockInfo_.size_ + OAConfig_.PadBytes_)
        / OAConfig_.Alignment_) + (PTR_SIZE + OAConfig_.HBlockInfo_.size_ + OAConfig_.PadBytes_) % OAConfig_.PadBytes_ == 0 ? 0 : 1)
            : PTR_SIZE + OAConfig_.HBlockInfo_.size_ + OAConfig_.PadBytes_;

    this->dataSize =
        OAConfig_.Alignment_ ? OAConfig_.Alignment_ * (((ObjectSize + OAConfig_.PadBytes_ * 2 + OAConfig_.HBlockInfo_.size_)
        / OAConfig_.Alignment_) + (ObjectSize + OAConfig_.PadBytes_ * 2 + OAConfig_.HBlockInfo_.size_) % OAConfig_.PadBytes_ == 0 ? 0 : 1)
            : ObjectSize + OAConfig_.PadBytes_ * 2 + OAConfig_.HBlockInfo_.size_;
}

ObjectAllocator::~ObjectAllocator(){
}

void* ObjectAllocator::Allocate(const char *label){
    if (FreeList_==nullptr)
    {
        // size_t pageSize_ = this->OAStats_.PageSize_; 
        // GenericObject* startNew = reinterpret_cast<GenericObject*>(new unsigned char[pageSize_]()) ;
        // this->OAStats_.PagesInUse_ += 1;
        // startNew->Next = this->PageList_;

        if (OAStats_.PagesInUse_ == OAConfig_.MaxPages_){
            // Max pages have been reached.
            throw OAException(OAException::E_NO_PAGES, "No more pages");
         }

        //check if can allocate new page
        else if(this->PageList_ == nullptr){
            //if can, allocate new page
            GenericObject* newObj;
            GenericObject* newP;
            try{
                newObj = reinterpret_cast<GenericObject*>
                                        (new unsigned char[this->OAStats_.PageSize_]());
                ++this->OAStats_.PagesInUse_;
                newP = newObj;
            }
            catch (std::bad_alloc& exception){
                throw OAException(OAException::E_NO_MEMORY, "No more memory");
            }

            if (this->OAConfig_.DebugOn_){
            memset(newP, ALIGN_PATTERN, this->OAStats_.PageSize_);
            }
            
            //set newpage as new head, replacing pagelist (pageList,newPage)
            newP->Next = this->PageList_;
            this->PageList_ = newP;

            //give newPage an address
            unsigned char* PageStartAddress = reinterpret_cast<unsigned char*>(newP);
            //give the data a starting address (newpageaddress+headersize)
            unsigned char* DataStartAddress = PageStartAddress + this->headerSize;

            //alloc space for obj on freelist
            for (; static_cast<unsigned>(abs(static_cast<int>(DataStartAddress - PageStartAddress))) < this->OAStats_.PageSize_;
            DataStartAddress += this->dataSize)
            {
            // We intepret it as a pointer.
            GenericObject* dataAddress = reinterpret_cast<GenericObject*>(DataStartAddress);

            // Add the pointer to the free list.
            GenericObject* temp = this->FreeList_;
            this->FreeList_ = dataAddress;
            dataAddress->Next = temp;
            this->OAStats_.FreeObjects_++;

            if (this->OAConfig_.DebugOn_)
            {
                // Update padding sig
                memset(reinterpret_cast<unsigned char*>(dataAddress) + PTR_SIZE, UNALLOCATED_PATTERN, this->OAStats_.ObjectSize_ - PTR_SIZE);
                memset(reinterpret_cast<unsigned char*>(dataAddress) - this->OAConfig_.PadBytes_, PAD_PATTERN, this->OAConfig_.PadBytes_);
                memset(reinterpret_cast<unsigned char*>(dataAddress) + this->OAStats_.ObjectSize_, PAD_PATTERN, this->OAConfig_.PadBytes_);
            }
            memset(reinterpret_cast<unsigned char*>(dataAddress) - this->OAConfig_.PadBytes_ - this->OAConfig_.HBlockInfo_.size_, 0, OAConfig_.HBlockInfo_.size_);
            }
        }
    }
    GenericObject* allocObj = this->FreeList_;
        // Update the free list
        this->FreeList_ = this->FreeList_->Next;
        // Update memory sig
        if (this->OAConfig_.DebugOn_)
        {
            memset(allocObj, ALLOCATED_PATTERN, OAStats_.ObjectSize_);
        }

        // Update stats
        ++this->OAStats_.ObjectsInUse_;
        if (this->OAStats_.ObjectsInUse_ > this->OAStats_.MostObjects_)
        this->OAStats_.MostObjects_ = this->OAStats_.ObjectsInUse_;
        --this->OAStats_.FreeObjects_;
        ++this->OAStats_.Allocations_;

        // Update header
        if(OAConfig_.HBlockInfo_.type_ == OAConfig::hbBasic){
            unsigned char* headerAddr = reinterpret_cast<unsigned char*>(allocObj) - this->OAConfig_.PadBytes_ - this->OAConfig_.HBlockInfo_.size_;
            unsigned* allocationNumber = reinterpret_cast<unsigned*>(headerAddr);
            *allocationNumber = this->OAStats_.Allocations_;
            // Now set the allocation flag
            unsigned char* flag = reinterpret_cast<unsigned char*>(INCREMENT_PTR(allocationNumber));
            *flag = true;
        }
        else if(OAConfig_.HBlockInfo_.type_ == OAConfig::hbExtended){
            unsigned char* headerAddr = reinterpret_cast<unsigned char*>(allocObj) - this->OAConfig_.PadBytes_ - this->OAConfig_.HBlockInfo_.size_;
            // Set the 2 byte use-counter, 5 for 5 bytes of user defined stuff.
            unsigned short* counter = reinterpret_cast<unsigned short*>(headerAddr + this->OAConfig_.HBlockInfo_.additional_);
            ++(*counter);
            
            unsigned* allocationNumber = reinterpret_cast<unsigned*>(INCREMENT_PTR(counter));
            *allocationNumber = this->OAStats_.Allocations_;
            // Now set the allocation flag
            unsigned char* flag = reinterpret_cast<unsigned char*>(INCREMENT_PTR(allocationNumber));
            *flag = true;
        }
        else if(OAConfig_.HBlockInfo_.type_ == OAConfig::hbExternal){
            unsigned char* headerAddr = reinterpret_cast<unsigned char*>(allocObj) - this->OAConfig_.PadBytes_ - this->OAConfig_.HBlockInfo_.size_;
            MemBlockInfo** memPtr = reinterpret_cast<MemBlockInfo**>(headerAddr);
            try
            {
                *memPtr = new MemBlockInfo;
                (*memPtr) -> alloc_num = OAStats_.Allocations_;
                (*memPtr) ->label = const_cast<char*>(label);
            }
            catch (std::bad_alloc&)
            {
                throw OAException(OAException::E_NO_MEMORY, "No memory");
            }
        }

        return allocObj;
}

void ObjectAllocator::Free(void *Object){

}

// unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const {

// }

// unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const {

// }

// unsigned ObjectAllocator::FreeEmptyPages(){

// }

// void ObjectAllocator::SetDebugState(bool State){

// }

const void* ObjectAllocator::GetFreeList() const{
    return FreeList_;
}

const void* ObjectAllocator::GetPageList() const{
    return PageList_;
}

OAConfig ObjectAllocator::GetConfig() const{
    return this->OAConfig_;
}

OAStats ObjectAllocator::GetStats() const{
    return this->OAStats_;
}