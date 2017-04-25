/*
 *  (c) Copyright 2016-2017 Hewlett Packard Enterprise Development Company LP.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <assert.h> // for assert()
#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED

#include "nvmm/error_code.h"
#include "nvmm/global_ptr.h"
#include "shelf_mgmt/shelf_file.h"

#include "nvmm/log.h"
#include "shelf_usage/zone.h"

#include "shelf_usage/zone_shelf_heap.h"

namespace nvmm {

ShelfHeap::ShelfHeap(std::string pathname)
    : is_open_{false}, shelf_{pathname}, addr_{NULL}, zone_{NULL}
{
}

ShelfHeap::ShelfHeap(std::string pathname, ShelfId shelf_id)
    : is_open_{false}, shelf_{pathname, shelf_id}, addr_{NULL}, zone_{NULL}
{
}
    
ShelfHeap::~ShelfHeap()
{
    if (IsOpen() == true)
    {
        (void)Close();
    }
}

ErrorCode ShelfHeap::Create(size_t zone_size, void *helper, size_t helper_size)
{
    assert(IsOpen() == false);
    assert(shelf_.Exist() == true);
    
    ErrorCode ret = NO_ERROR;    
    
    // allocate memory for the shelf
    // TODO: make sure actual_size is rounded up to N*8GB
    ret = shelf_.Truncate(zone_size);
    if (ret != NO_ERROR)
    {
        return ret;
    }
    
    // then, format the shelf
    ret = OpenMapShelf();
    if (ret != NO_ERROR)
    {
        return ret;
    }
    
    // create zone layout
    // TODO: this will fail if the shelf file already exists; if the file exists and it is
    // already inited, it will fail
    Zone *zone = new Zone(addr_, zone_size, 64, zone_size, helper, helper_size);
    delete zone;
    
    ret= UnmapCloseShelf();
    if (ret != NO_ERROR)
    {
        return ret;
    }        
    
    return ret;
}
        
ErrorCode ShelfHeap::Destroy()
{
    assert(IsOpen() == false);

    ErrorCode ret = NO_ERROR;

    ret = OpenMapShelf(true);
    if (ret != NO_ERROR)
    {
        return ret;
    }
    
    // destroy the bitmap
    // TODO: zero out everything 
    
    ret = UnmapCloseShelf(true, true);
    if (ret != NO_ERROR)
    {
        return ret;
    }        

    // free space for the shelf
    return shelf_.Truncate(0);
}

ErrorCode ShelfHeap::Verify()
{
    assert(IsOpen() == false);
    ErrorCode ret = NO_ERROR;
    ret = OpenMapShelf();
    if (ret != NO_ERROR)
    {
        return ret;
    }
    
    // TODO: verify zone header

    //out:    
    (void) UnmapCloseShelf();
    return ret;
}

ErrorCode ShelfHeap::Recover()
{
    // TODO: not yet implemented
    return NO_ERROR;
}
    
ErrorCode ShelfHeap::Open(void *helper, size_t helper_size)
{
    assert(IsOpen() == false);

    ErrorCode ret = NO_ERROR;

    ret = OpenMapShelf(true);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    // TODO: verify zone header
    helper_ = helper;
    helper_size_ = helper_size;
    zone_ = new Zone(addr_, shelf_.Size(), helper_, helper_size_);

    is_open_ = true;
    return ret;
}

ErrorCode ShelfHeap::Close()
{
    assert(IsOpen() == true);

    delete zone_;
    zone_ = NULL;
    
    ErrorCode ret = UnmapCloseShelf(true, false);
    if (ret == NO_ERROR)
    {
        is_open_ = false;
    }
    return ret;
}

size_t ShelfHeap::Size()
{
    assert(IsOpen() == true);
    return shelf_.Size();
}

size_t ShelfHeap::MinAllocSize()
{
    assert(IsOpen() == true);
    return zone_->min_obj_size();
}

Offset ShelfHeap::Alloc(size_t size)
{
    assert(IsOpen() == true);
    Offset offset;
    offset = (Offset)zone_->alloc(size);
    LOG(trace) << "ShelfHeap::Alloc " << offset;
    return offset;
}

void ShelfHeap::Free(Offset offset)
{
    assert(IsOpen() == true);
    zone_->free(offset);
    LOG(trace) << "ShelfHeap::Free " << offset;
}

bool ShelfHeap::IsValidOffset(Offset offset)
{
    assert(IsOpen() == true);
    return zone_->IsValidOffset(offset);
}

void *ShelfHeap::OffsetToPtr(Offset offset) const
{
    assert(IsOpen() == true);
    assert(zone_->IsValidOffset(offset) == true);
    return zone_->OffsetToPtr(offset);
}

Offset ShelfHeap::PtrToOffset(void *addr)
{
    assert(IsOpen() == true);
    assert(addr>zone_);
    Offset offset = (char*)addr - (char*)zone_;
    assert(zone_->IsValidOffset(offset) == true);
    return offset;
}

ErrorCode ShelfHeap::OpenMapShelf(bool use_shelf_manager)
{
    // check if the shelf exists
    if (shelf_.Exist() == false)
    {
        return SHELF_FILE_NOT_FOUND;
    }

    ErrorCode ret = NO_ERROR;

    // open the shelf
    ret = shelf_.Open(O_RDWR);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    // mmap the shelf
    assert(addr_ == NULL);
    if (use_shelf_manager == true)
    {
        ret = shelf_.Map(NULL, &addr_);
    }
    else
    {
        size_t size = shelf_.Size();
        if (size != 0)
        {
            ret = shelf_.Map(NULL, size , PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr_, true);
            if (ret == NO_ERROR)
            {
                assert(addr_ != NULL);
            }
            else
            {
                return ret;
            }
        }
    }
    return ret;
}
    
ErrorCode ShelfHeap::UnmapCloseShelf(bool use_shelf_manager, bool unregister)
{
    // check if the shelf exists
    if (shelf_.Exist() == false)
    {
        return SHELF_FILE_NOT_FOUND;
    }

    ErrorCode ret = NO_ERROR;

    // unmmap the shelf
    assert(addr_ != NULL);
    size_t size = shelf_.Size();
    if (use_shelf_manager == true)
    {
        ret = shelf_.Unmap(addr_, unregister);
    }
    else
    {
        ret = shelf_.Unmap(addr_, size, true);
    }
    if (ret == NO_ERROR)
    {
        addr_ = NULL;
    }
    else
    {
        return ret;
    }
        
    // close the shelf
    ret = shelf_.Close();
    return ret;        
}

} // namespace nvmm
