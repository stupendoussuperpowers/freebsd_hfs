/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
	File:		BTreeAllocate.c

	Contains:	BTree Node Allocation routines for the BTree Module.

	Version:	xxx put the technology version here xxx

	Written by:	Gordon Sheridan and Bill Bruffey

	Copyright:	� 1992-1999 by Apple Computer, Inc., all rights reserved.

	File Ownership:

		DRI:				Don Brady

		Other Contact:		Mark Day

		Technology:			File Systems

	Writers:

		(djb)	Don Brady
		(ser)	Scott Roberts
		(msd)	Mark Day

	Change History (most recent first):

	   <MOSXS>	  6/1/99	djb		Sync up with Mac OS 8.6.
	   <CS3>	11/24/97	djb		Remove some debug code (Panic calls).
	   <CS2>	 7/24/97	djb		CallbackProcs now take refnum instead of an FCB.
	   <CS1>	 4/23/97	djb		first checked in

	  <HFS2>	 2/19/97	djb		Change E_BadNodeType to fsBTBadNodeType.
	  <HFS1>	12/19/96	djb		first checked in

	History applicable to original Scarecrow Design:

		 <4>	10/25/96	ser		Changing for new VFPI
		 <3>	10/18/96	ser		Converting over VFPI changes
		 <2>	 1/10/96	msd		Change 64-bit math to use real function names from Math64.i.
		 <1>	10/18/95	rst		Moved from Scarecrow project.

		 <8>	 1/12/95	wjk		Adopt Model FileSystem changes in D5.
		 <7>	 9/30/94	prp		Get in sync with D2 interface changes.
		 <6>	 7/22/94	wjk		Convert to the new set of header files.
		 <5>	 8/31/93	prp		Use U64SetU instead of S64Set.
		 <4>	 5/21/93	gs		Fix ExtendBTree bug.
		 <3>	 5/10/93	gs		Fix pointer arithmetic bug in AllocateNode.
		 <2>	 3/23/93	gs		finish ExtendBTree routine.
		 <1>	  2/8/93	gs		first checked in
		 <0>	  1/1/93	gs		begin AllocateNode and FreeNode

*/

#include <sys/types.h>

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_endian.h>

#include "../headers/BTreesPrivate.h"

///////////////////// Routines Internal To BTreeAllocate.c //////////////////////

OSStatus GetMapNode(BTreeControlBlockPtr btreePtr, BlockDescriptor *nodePtr, UInt16 **mapPtr, UInt16 *mapSize);

/////////////////////////////////////////////////////////////////////////////////

/*-------------------------------------------------------------------------------

Routine:	AllocateNode	-	Find Free Node, Mark It Used, and Return Node Number.

Function:	Searches the map records for the first free node, marks it "in use" and
			returns the node number found. This routine should really only be called
			when we know there are free blocks, otherwise it's just a waste of time.

Note:		We have to examine map nodes a word at a time rather than a long word
			because the External BTree Mgr used map records that were not an integral
			number of long words. Too bad. In our spare time could develop a more
			sophisticated algorithm that read map records by long words (and long
			word aligned) and handled the spare bytes at the beginning and end
			appropriately.

Input:		btreePtr	- pointer to control block for BTree file

Output:		nodeNum		- number of node allocated


Result:		noErr			- success
			fsBTNoMoreMapNodesErr	- no free blocks were found
			!= noErr		- failure
-------------------------------------------------------------------------------*/

OSStatus
AllocateNode(BTreeControlBlockPtr btreePtr, UInt32 *nodeNum)
{
	OSStatus err;
	BlockDescriptor node;
	UInt16 *mapPtr, *pos;
	UInt16 mapSize, size;
	UInt16 freeWord;
	UInt16 mask;
	UInt16 bitOffset;
	UInt32 nodeNumber;

	nodeNumber = 0;	   // first node number of header map record
	node.buffer = nil; // clear node.buffer to get header node
			   //	- and for ErrorExit
	node.blockHeader = nil;

	while (true) {
		err = GetMapNode(btreePtr, &node, &mapPtr, &mapSize);
		M_ExitOnError(err);

		// XXXdbg
		ModifyBlockStart(btreePtr->fileRefNum, &node);

		//////////////////////// Find Word with Free Bit ////////////////////////////

		pos = mapPtr;
		size = mapSize;
		size >>= 1; // convert to number of words
			    // �� assumes mapRecords contain an integral number of words

		while (size--) {
			if (*pos++ != 0xFFFF) // assume test fails, and increment pos
				break;
		}

		--pos; // whoa! backup

		if (*pos != 0xFFFF) // hey, we got one!
			break;

		nodeNumber += mapSize << 3; // covert to number of bits (nodes)
	}

	///////////////////////// Find Free Bit in Word /////////////////////////////

	freeWord = SWAP_BE16(*pos);
	bitOffset = 15;
	mask = 0x8000;

	do {
		if ((freeWord & mask) == 0)
			break;
		mask >>= 1;
	} while (--bitOffset);

	////////////////////// Calculate Free Node Number ///////////////////////////

	nodeNumber += ((pos - mapPtr) << 4) + (15 - bitOffset); // (pos-mapPtr) = # of words!

	///////////////////////// Check for End of Map //////////////////////////////

	if (nodeNumber >= btreePtr->totalNodes) {
		err = fsBTFullErr;
		goto ErrorExit;
	}

	/////////////////////////// Allocate the Node ///////////////////////////////

	*pos |= SWAP_BE16(mask); // set the map bit for the node

	err = UpdateNode(btreePtr, &node, 0, kLockTransaction);
	M_ExitOnError(err);

	--btreePtr->freeNodes;
	btreePtr->flags |= kBTHeaderDirty;
	*nodeNum = nodeNumber;

	return noErr;

	////////////////////////////////// Error Exit ///////////////////////////////////

ErrorExit:

	(void)ReleaseNode(btreePtr, &node);
	*nodeNum = 0;

	return err;
}

/*-------------------------------------------------------------------------------

Routine:	FreeNode	-	Clear allocation bit for node.

Function:	Finds the bit representing the node specified by nodeNum in the node
			map and clears the bit.


Input:		btreePtr	- pointer to control block for BTree file
			nodeNum		- number of node to mark free

Output:		none

Result:		noErr			- success
			fsBTNoMoreMapNodesErr	- node number is beyond end of node map
			!= noErr		- GetNode or ReleaseNode encountered some difficulty
-------------------------------------------------------------------------------*/

OSStatus
FreeNode(BTreeControlBlockPtr btreePtr, UInt32 nodeNum)
{
	OSStatus err;
	BlockDescriptor node;
	UInt32 nodeIndex;
	UInt16 mapSize;
	UInt16 *mapPos;
	UInt16 bitOffset;

	//////////////////////////// Find Map Record ////////////////////////////////
	nodeIndex = 0;	   // first node number of header map record
	node.buffer = nil; // invalidate node.buffer to get header node
	node.blockHeader = nil;

	while (nodeNum >= nodeIndex) {
		err = GetMapNode(btreePtr, &node, &mapPos, &mapSize);
		M_ExitOnError(err);

		nodeIndex += mapSize << 3; // covert to number of bits (nodes)
	}

	//////////////////////////// Mark Node Free /////////////////////////////////

	// XXXdbg
	ModifyBlockStart(btreePtr->fileRefNum, &node);

	nodeNum -= (nodeIndex - (mapSize << 3)); // relative to this map record
	bitOffset = 15 - (nodeNum & 0x0000000F); // last 4 bits are bit offset
	mapPos += nodeNum >> 4;			 // point to word containing map bit

	M_SWAP_BE16_ClearBitNum(*mapPos, bitOffset); // clear it

	err = UpdateNode(btreePtr, &node, 0, kLockTransaction);
	M_ExitOnError(err);

	++btreePtr->freeNodes;
	btreePtr->flags |= kBTHeaderDirty; // how about a macro for this

	return noErr;

ErrorExit:

	(void)ReleaseNode(btreePtr, &node);

	return err;
}

/*-------------------------------------------------------------------------------

Routine:	ExtendBTree	-	Call FSAgent to extend file, and allocate necessary map nodes.

Function:	This routine calls the the FSAgent to extend the end of fork, if necessary,
			to accomodate the number of nodes requested. It then allocates as many
			map nodes as are necessary to account for all the nodes in the B*Tree.
			If newTotalNodes is less than the current number of nodes, no action is
			taken.

Note:		Internal HFS File Manager BTree Module counts on an integral number of
			long words in map records, although they are not long word aligned.

Input:		btreePtr		- pointer to control block for BTree file
			newTotalNodes	- total number of nodes the B*Tree is to extended to

Output:		none

Result:		noErr		- success
			!= noErr	- failure
-------------------------------------------------------------------------------*/

OSStatus
ExtendBTree(BTreeControlBlockPtr btreePtr, UInt32 newTotalNodes)
{
	OSStatus err;
	FCB *filePtr;
	FSSize minEOF, maxEOF;
	UInt16 nodeSize;
	UInt32 oldTotalNodes;
	UInt32 newMapNodes;
	UInt32 mapBits, totalMapBits;
	UInt32 recStartBit;
	UInt32 nodeNum, nextNodeNum;
	UInt32 firstNewMapNodeNum, lastNewMapNodeNum;
	BlockDescriptor mapNode, newNode;
	UInt16 *mapPos;
	UInt16 *mapStart;
	UInt16 mapSize;
	UInt16 mapNodeRecSize;
	UInt32 bitInWord, bitInRecord;
	UInt16 mapIndex;

	oldTotalNodes = btreePtr->totalNodes;
	if (newTotalNodes <= oldTotalNodes) // we're done!
		return noErr;

	nodeSize = btreePtr->nodeSize;
	filePtr = GetFileControlBlock(btreePtr->fileRefNum);

	mapNode.buffer = nil;
	mapNode.blockHeader = nil;
	newNode.buffer = nil;
	newNode.blockHeader = nil;

	mapNodeRecSize = nodeSize - sizeof(BTNodeDescriptor) - 6; // 2 bytes of free space (see note)

	//////////////////////// Count Bits In Node Map /////////////////////////////

	totalMapBits = 0;
	do {
		err = GetMapNode(btreePtr, &mapNode, &mapStart, &mapSize);
		M_ExitOnError(err);

		mapBits = mapSize << 3;	    // mapSize (in bytes) * 8
		recStartBit = totalMapBits; // bit number of first bit in map record
		totalMapBits += mapBits;

	} while (((BTNodeDescriptor *)mapNode.buffer)->fLink != 0);

	if (DEBUG_BUILD && totalMapBits != CalcMapBits(btreePtr))
		Panic("ExtendBTree: totalMapBits != CalcMapBits");

	/////////////////////// Extend LEOF If Necessary ////////////////////////////

	minEOF = (UInt64)newTotalNodes * (UInt64)nodeSize;
	if (filePtr->fcbEOF < minEOF) {
		maxEOF = (UInt64)0x7fffffffLL * (UInt64)nodeSize;

		err = btreePtr->setEndOfForkProc(btreePtr->fileRefNum, minEOF, maxEOF);
		M_ExitOnError(err);
	}

	//////////////////// Calc New Total Number Of Nodes /////////////////////////

	newTotalNodes = filePtr->fcbEOF / nodeSize; // hack!
	// do we wish to perform any verification of newTotalNodes at this point?

	btreePtr->totalNodes = newTotalNodes; // do we need to update freeNodes here too?

	////////////// Calculate Number Of New Map Nodes Required ///////////////////

	newMapNodes = 0;
	if (newTotalNodes > totalMapBits) {
		newMapNodes = (((newTotalNodes - totalMapBits) >> 3) / mapNodeRecSize) + 1;
		firstNewMapNodeNum = oldTotalNodes;
		lastNewMapNodeNum = firstNewMapNodeNum + newMapNodes - 1;
	} else {
		err = ReleaseNode(btreePtr, &mapNode);
		M_ExitOnError(err);

		goto Success;
	}

	/////////////////////// Initialize New Map Nodes ////////////////////////////
	// XXXdbg - this is the correct place for this:
	ModifyBlockStart(btreePtr->fileRefNum, &mapNode);

	((BTNodeDescriptor *)mapNode.buffer)->fLink = firstNewMapNodeNum;

	nodeNum = firstNewMapNodeNum;
	while (true) {
		err = GetNewNode(btreePtr, nodeNum, &newNode);
		M_ExitOnError(err);

		// XXXdbg
		ModifyBlockStart(btreePtr->fileRefNum, &newNode);

		((NodeDescPtr)newNode.buffer)->numRecords = 1;
		((NodeDescPtr)newNode.buffer)->kind = kBTMapNode;

		// set free space offset
		*(UInt16 *)((Ptr)newNode.buffer + nodeSize - 4) = nodeSize - 6;

		if (nodeNum++ == lastNewMapNodeNum)
			break;

		((BTNodeDescriptor *)newNode.buffer)->fLink = nodeNum; // point to next map node

		err = UpdateNode(btreePtr, &newNode, 0, kLockTransaction);
		M_ExitOnError(err);
	}

	err = UpdateNode(btreePtr, &newNode, 0, kLockTransaction);
	M_ExitOnError(err);

	///////////////////// Mark New Map Nodes Allocated //////////////////////////

	nodeNum = firstNewMapNodeNum;
	do {
		bitInRecord = nodeNum - recStartBit;

		while (bitInRecord >= mapBits) {
			nextNodeNum = ((NodeDescPtr)mapNode.buffer)->fLink;
			if (nextNodeNum == 0) {
				err = fsBTNoMoreMapNodesErr;
				goto ErrorExit;
			}

			err = UpdateNode(btreePtr, &mapNode, 0, kLockTransaction);
			M_ExitOnError(err);

			err = GetNode(btreePtr, nextNodeNum, &mapNode);
			M_ExitOnError(err);

			// XXXdbg
			ModifyBlockStart(btreePtr->fileRefNum, &mapNode);

			mapIndex = 0;

			mapStart = (UInt16 *)GetRecordAddress(btreePtr, mapNode.buffer, mapIndex);
			mapSize = GetRecordSize(btreePtr, mapNode.buffer, mapIndex);

			if (DEBUG_BUILD && mapSize != M_MapRecordSize(btreePtr->nodeSize)) {
				Panic("ExtendBTree: mapSize != M_MapRecordSize");
			}

			mapBits = mapSize << 3;	    // mapSize (in bytes) * 8
			recStartBit = totalMapBits; // bit number of first bit in map record
			totalMapBits += mapBits;

			bitInRecord = nodeNum - recStartBit;
		}

		mapPos = mapStart + ((nodeNum - recStartBit) >> 4);
		bitInWord = 15 - ((nodeNum - recStartBit) & 0x0000000F);

		M_SWAP_BE16_SetBitNum(*mapPos, bitInWord);

		++nodeNum;

	} while (nodeNum <= lastNewMapNodeNum);

	err = UpdateNode(btreePtr, &mapNode, 0, kLockTransaction);
	M_ExitOnError(err);

	//////////////////////////////// Success ////////////////////////////////////

Success:

	btreePtr->totalNodes = newTotalNodes;
	btreePtr->freeNodes += (newTotalNodes - oldTotalNodes) - newMapNodes;

	btreePtr->flags |= kBTHeaderDirty; // �� how about a macro for this

	/* Force the b-tree header changes to disk */
	(void)UpdateHeader(btreePtr, true);

	return noErr;

	////////////////////////////// Error Exit ///////////////////////////////////

ErrorExit:

	(void)ReleaseNode(btreePtr, &mapNode);
	(void)ReleaseNode(btreePtr, &newNode);

	return err;
}

/*-------------------------------------------------------------------------------

Routine:	GetMapNode	-	Get the next map node and pointer to the map record.

Function:	Given a BlockDescriptor to a map node in nodePtr, GetMapNode releases
			it and gets the next node. If nodePtr->buffer is nil, then the header
			node is retrieved.


Input:		btreePtr	- pointer to control block for BTree file
			nodePtr		- pointer to a BlockDescriptor of a map node

Output:		nodePtr		- pointer to the BlockDescriptor for the next map node
			mapPtr		- pointer to the map record within the map node
			mapSize		- number of bytes in the map record

Result:		noErr			- success
			fsBTNoMoreMapNodesErr	- we've run out of map nodes
			fsBTInvalidNodeErr			- bad node, or not node type kMapNode
			!= noErr		- failure
-------------------------------------------------------------------------------*/

OSStatus
GetMapNode(BTreeControlBlockPtr btreePtr, BlockDescriptor *nodePtr, UInt16 **mapPtr, UInt16 *mapSize)
{
	OSStatus err;
	UInt16 mapIndex;
	UInt32 nextNodeNum;

	if (nodePtr->buffer != nil) // if iterator is valid...
	{
		nextNodeNum = ((NodeDescPtr)nodePtr->buffer)->fLink;
		if (nextNodeNum == 0) {
			err = fsBTNoMoreMapNodesErr;
			goto ErrorExit;
		}

		err = ReleaseNode(btreePtr, nodePtr);
		M_ExitOnError(err);

		err = GetNode(btreePtr, nextNodeNum, nodePtr);
		M_ExitOnError(err);

		if (((NodeDescPtr)nodePtr->buffer)->kind != kBTMapNode) {
			err = fsBTBadNodeType;
			goto ErrorExit;
		}

		++btreePtr->numMapNodesRead;
		mapIndex = 0;
	} else {
		err = GetNode(btreePtr, kHeaderNodeNum, nodePtr);
		M_ExitOnError(err);

		if (((NodeDescPtr)nodePtr->buffer)->kind != kBTHeaderNode) {
			err = fsBTInvalidHeaderErr; // �� or fsBTBadNodeType
			goto ErrorExit;
		}

		mapIndex = 2;
	}

	*mapPtr = (UInt16 *)GetRecordAddress(btreePtr, nodePtr->buffer, mapIndex);
	*mapSize = GetRecordSize(btreePtr, nodePtr->buffer, mapIndex);

	return noErr;

ErrorExit:

	(void)ReleaseNode(btreePtr, nodePtr);

	*mapPtr = nil;
	*mapSize = 0;

	return err;
}

////////////////////////////////// CalcMapBits //////////////////////////////////

UInt32
CalcMapBits(BTreeControlBlockPtr btreePtr)
{
	UInt32 mapBits;

	mapBits = M_HeaderMapRecordSize(btreePtr->nodeSize) << 3;

	while (mapBits < btreePtr->totalNodes)
		mapBits += M_MapRecordSize(btreePtr->nodeSize) << 3;

	return mapBits;
}
