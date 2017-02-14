/*
 * Copyright 2017, International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "action_hashjoin_hls.H"

using namespace std;

// ----------------------------------------------------------------------------
// Known Limitations => Issue #39 & #45
//      => Transfers must be 64 byte aligned and a size of multiples of 64 bytes
// ----------------------------------------------------------------------------
// v1.4 : 01/20/2017 :  cleaning code             : split files and link them
// v1.3 : 01/17/2017 :  simplifying code          : read tables sent by application
//                      HLS_SYN_MEM=142,HLS_SYN_DSP=26,HLS_SYN_FF=20836,HLS_SYN_LUT=32321
// v1.2 : 01/10/2017 :  adapting to master branch : change interfaces names and MEMDW=512
// v1.1 : 12/08/2016 :  adding 2nd C code         : HJ2 for real hash table creation
// v1.0 : 12/05/2016 :  creation from search V1.8 : HJ1 used for RD/WR database value
//                                                : + test string+int conversion

/*
 * WRITE RESULTS IN MMIO REGS
 *
 * Always check that ALL Outputs are tied to a value or HLS will generate a
 * Action_Output_i and a Action_Output_o registers and address to read results
 * will be shifted ...and wrong
 * => easy checking in generated files : grep 0x184 action_wrapper_ctrl_reg_s_axi.vhd
 * this grep should return nothing if no duplication of registers (which is expected)
 */
static void write_results_in_HJ_regs(action_output_reg *Action_Output,
			      action_input_reg *Action_Input,
			      snapu32_t ReturnCode,
			      snapu64_t field1,
			      snapu64_t field2,
			      snapu64_t field3,
			      snapu64_t field4)
{
	Action_Output->Retc     = (snapu32_t)ReturnCode;
	Action_Output->Reserved = (snapu64_t)0x0;

	Action_Output->Data.t1_processed   = field1;
	Action_Output->Data.t2_processed   = field2;
	Action_Output->Data.t3_produced    = field3;
	Action_Output->Data.checkpoint     = field4;
	Action_Output->Data.rc             = 0;
	Action_Output->Data.action_version =  RELEASE_VERSION;

	// Registers unchanged
	Action_Output->Data.t1 = Action_Input->Data.t1;
	Action_Output->Data.t2 = Action_Input->Data.t2;
	Action_Output->Data.t3 = Action_Input->Data.t3;
	Action_Output->Data.hash_table = Action_Input->Data.hash_table;
}

/*
 * FIXME The table1, 2, 3 read and write code, I consider a problem. We
 *       are doing what the HLS tool should do for us in a less error
 *       prone way. We need to know about the bit positions and byte
 *       ordering of the individual fields. Those details should be known
 *       to the tool. Once the table format changes, there is a lot of code
 *       to be reviewed or worse to be changed, e.g. if one adds an element
 *       before the existing ones.
 *
 * FIXME We like to get rid of this bit cutting out code. The tool should solve
 *       this problem and avoid the user to make mistakes there.
 */
static void copy_hashkey(snap_membus_t mem, hashkey_t key)
{
 loop_copy_hashkey:
	for (unsigned int k = 0; k < sizeof(hashkey_t); k++)
		key[k] = mem(8 * (k+1) - 1,  8 * k);
}

static snap_membus_t hashkey_to_mbus(hashkey_t key)
{
	snap_membus_t mem = 0;

 loop_hashkey_to_mbus:
	for (unsigned int k = 0; k < sizeof(hashkey_t); k++) {
		mem(8 * (k+1) - 1,  8 * k) = key[k];
	}
	return mem;
}

/*
 * Instead of reading each snap_membus_t line individually, we try to
 * read always a 4KiB block and return the line from the internal buffer.
 * if the buffer is empty, we read the following 4KiB block from the bus
 * in a burst.
 *
 * FIXME No underrun or access out of bounds protection.
 */
#define SNAP_4KiB_WORDS (4096 / sizeof(snap_membus_t))

typedef struct snap_4KiB_t {
	snap_membus_t buf[SNAP_4KiB_WORDS]; /* temporary storage buffer */
	snap_membus_t *mem;                 /* source where data comes from */
	unsigned int max_lines;             /* size of the memory buffer */
	unsigned int m_idx;                 /* read position for source */
	unsigned int b_idx;                 /* read position for buffer */
} snap_4KiB_t;

static void snap_4KiB_rinit(snap_4KiB_t *buf, snap_membus_t *mem,
			    unsigned int max_lines)
{
	buf->mem = mem;
	buf->max_lines = max_lines;
	buf->m_idx = 0;
	buf->b_idx = SNAP_4KiB_WORDS;
}

static void snap_4KiB_winit(snap_4KiB_t *buf, snap_membus_t *mem,
			    unsigned int max_lines)
{
	buf->mem = mem;
	buf->max_lines = max_lines;
	buf->m_idx = 0;
	buf->b_idx = 0;
}

/**
 * Reading beyond the available memory is blocked, buffer filling
 * is returned to keep code simple, and contains unuseable data.
 */
static void snap_4KiB_get(snap_4KiB_t *buf, snap_membus_t *line)
{
	if ((buf->m_idx == buf->max_lines) && (buf->b_idx == SNAP_4KiB_WORDS)) {
		*line = (snap_membus_t)-1;
		return;
	}
	/* buffer is empty, read in the next 4KiB */
	if (buf->b_idx == SNAP_4KiB_WORDS) {
		unsigned int tocopy =
			MIN(SNAP_4KiB_WORDS, buf->max_lines - buf->m_idx);

		memcpy(buf->buf, buf->mem + buf->m_idx,
		       tocopy * sizeof(snap_membus_t));
		
		buf->m_idx += tocopy;
		buf->b_idx = 0; /* buffer is full again */
	}
	*line = buf->buf[buf->b_idx];
	buf->b_idx++;
}

/**
 * Writing beyond the available memory is blocked, still we accept
 * data in the local buffer, but that will not be written out.
 */
static void snap_4KiB_flush(snap_4KiB_t *buf)
{
	unsigned int free_lines = buf->max_lines - buf->m_idx;
	unsigned int tocopy = MIN(free_lines, buf->b_idx);

	memcpy(buf->mem + buf->m_idx, buf->buf,
	       tocopy * sizeof(snap_membus_t));

	buf->m_idx += tocopy;
	buf->b_idx = 0;
}

static void snap_4KiB_put(snap_4KiB_t *buf, snap_membus_t line)
{
	/* buffer is full, flush the gathered 4KiB */
	if (buf->b_idx == SNAP_4KiB_WORDS) {
		snap_4KiB_flush(buf);
	}
	buf->buf[buf->b_idx] = line;
	buf->b_idx++;
}

static void read_table1(snap_membus_t *mem, unsigned int max_lines,
			t1_fifo_t *fifo1, uint32_t t1_used)
{
	unsigned int i;
	snap_4KiB_t buf;

	snap_4KiB_rinit(&buf, mem, max_lines);

 read_table1_loop:
	for (i = 0; i < t1_used; i++) {
#pragma HLS PIPELINE
		snap_membus_t b[2];
		table1_t t1;

		snap_4KiB_get(&buf, &b[0]);
		copy_hashkey(b[0], t1.name);

		snap_4KiB_get(&buf, &b[1]);
		t1.age = b[1](31, 0);

		fifo1->write(t1);
#if defined(CONFIG_FIFO_DEBUG)
		fprintf(stderr, "(K) fifo1->write(%d, %s)\n", i, t1.name);
#endif
	}
}

static void read_table2(snap_membus_t *mem, unsigned int max_lines,
			t2_fifo_t *fifo2, uint32_t t2_used)
{
	unsigned int i;
	snap_4KiB_t buf;

	snap_4KiB_rinit(&buf, mem, max_lines);

 read_table2_loop:
	for (i = 0; i < t2_used; i++) {
#pragma HLS PIPELINE
		snap_membus_t b[2];
		table2_t t2;

		snap_4KiB_get(&buf, &b[0]);
		copy_hashkey(b[0], t2.name);

		snap_4KiB_get(&buf, &b[1]);
		copy_hashkey(b[1], t2.animal);

		fifo2->write(t2);
#if defined(CONFIG_FIFO_DEBUG)
		fprintf(stderr, "(K) fifo2->write(%d, %s)\n", i, t2.name);
#endif
	}
}

static void write_table3(snap_membus_t *mem, unsigned int max_lines,
			 t3_fifo_t *fifo3, uint32_t t3_used)
{
	unsigned int i;
	snap_4KiB_t buf;

	snap_4KiB_winit(&buf, mem, max_lines);

	/* extract data into target table3, or FIFO maybe? */
 write_table3_loop:
	for (i = 0; i < t3_used; i++) {
		snap_membus_t d[3];
		table3_t t3 = fifo3->read();

#if defined(CONFIG_FIFO_DEBUG)
		fprintf(stderr, "(K) fifo3->read(%d, %s %s %d)\n",
			i, t3.name, t3.animal, t3.age);
#endif
		d[0] = hashkey_to_mbus(t3.animal);
		d[1] = hashkey_to_mbus(t3.name);
		d[2](511, 32) = 0;
		d[2](31, 0) = t3.age;

		snap_4KiB_put(&buf, d[0]);
		snap_4KiB_put(&buf, d[1]); 
		snap_4KiB_put(&buf, d[2]);
	}
	snap_4KiB_flush(&buf);
}

//-----------------------------------------------------------------------------
//--- MAIN PROGRAM ------------------------------------------------------------
//-----------------------------------------------------------------------------
void action_wrapper(snap_membus_t *din_gmem,
		    snap_membus_t *dout_gmem,
		    snap_membus_t *d_ddrmem,
		    action_input_reg *Action_Input,
		    action_output_reg *Action_Output)
{
	// Host Memory AXI Interface
#pragma HLS INTERFACE m_axi port=din_gmem bundle=host_mem
#pragma HLS INTERFACE m_axi port=dout_gmem bundle=host_mem
#pragma HLS INTERFACE s_axilite port=din_gmem bundle=ctrl_reg
#pragma HLS INTERFACE s_axilite port=dout_gmem bundle=ctrl_reg

	//DDR memory Interface
#pragma HLS INTERFACE m_axi port=d_ddrmem    bundle=card_mem0 offset=slave
#pragma HLS INTERFACE s_axilite port=d_ddrmem    bundle=ctrl_reg

	// Host Memory AXI Lite Master Interface
#pragma HLS DATA_PACK variable=Action_Input
#pragma HLS INTERFACE s_axilite port=Action_Input offset=0x080 bundle=ctrl_reg
#pragma HLS DATA_PACK variable=Action_Output
#pragma HLS INTERFACE s_axilite port=Action_Output offset=0x104 bundle=ctrl_reg
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl_reg

	// VARIABLES
	snapu16_t i, j;
	short rc;
	snapu32_t ReturnCode = 0;
	snapu64_t T1_address;
	snapu64_t T2_address;
	snapu64_t T3_address;
	snapu64_t T3_produced;
	snapu16_t T1_type;
	snapu16_t T2_type;
	snapu16_t T3_type;
	snapu32_t T1_size;
	snapu32_t T2_size;
	snapu32_t T3_size;
	unsigned int T1_items = 0;
	unsigned int T2_items = 0;
	unsigned int __table3_idx = 0;

#define pragma HLS DATAFLOW
	t1_fifo_t t1_fifo;
	t2_fifo_t t2_fifo;
	t3_fifo_t t3_fifo;
#pragma HLS stream variable=t1_fifo depth=4
#pragma HLS stream variable=t2_fifo depth=4
#pragma HLS stream variable=t3_fifo depth=4

	//== Parameters fetched in memory ==
	//==================================

	/*
	 * FIXME We added the do { } while (0) construct to avoid MMIO
	 * register duplication which we observed happening when we
	 * called write_results_in_HJ_regs() multiple times.
	 */
	do {
		/* FIXME Please check if the data alignment matches the expectations */
		if (Action_Input->Control.action != HASHJOIN_ACTION_TYPE) {
			ReturnCode = RET_CODE_FAILURE;
			break;
		}

		// byte address received need to be aligned with port width
		T1_address = Action_Input->Data.t1.address;
		T1_type    = Action_Input->Data.t1.type;
		T1_size    = Action_Input->Data.t1.size;
		T1_items   = T1_size / sizeof(table1_t);
		T2_address = Action_Input->Data.t2.address;
		T2_type    = Action_Input->Data.t2.type;
		T2_size    = Action_Input->Data.t2.size;
		T2_items   = T2_size / sizeof(table2_t);
		T3_address = Action_Input->Data.t3.address;
		T3_type    = Action_Input->Data.t3.type;
		T3_size    = Action_Input->Data.t3.size;
		ReturnCode = RET_CODE_OK;

		fprintf(stderr, "t1: %016lx/%08x t2: %016lx/%08x t3: %016lx/%08x\n",
			(long)T1_address, (int)T1_size,
			(long)T2_address, (int)T2_size,
			(long)T3_address, (int)T3_size);

		/* FIXME Just Host DDRAM for now */
		read_table1(din_gmem + (T1_address >> ADDR_RIGHT_SHIFT),
			    T1_size / sizeof(snap_membus_t),
			    &t1_fifo, T1_items);
		read_table2(din_gmem + (T2_address >> ADDR_RIGHT_SHIFT),
			    T2_size / sizeof(snap_membus_t),
			    &t2_fifo, T2_items);

		__table3_idx = 0;
		rc = action_hashjoin_hls(&t1_fifo, T1_items,
					 &t2_fifo, T2_items,
					 &t3_fifo, &__table3_idx);
		if (rc == 0) {
			/* FIXME Just Host DDRAM for now */
			write_table3(dout_gmem + (T3_address>>ADDR_RIGHT_SHIFT),
				     T3_size / sizeof(snap_membus_t),
				     &t3_fifo, __table3_idx);
		} else
			ReturnCode = RET_CODE_FAILURE;
	} while (0);

	write_results_in_HJ_regs(Action_Output, Action_Input, ReturnCode, 0, 0,
				 __table3_idx, 0);
}

//-----------------------------------------------------------------------------
//--- TESTBENCH ---------------------------------------------------------------
//-----------------------------------------------------------------------------

#if defined(NO_SYNTH)

/* table1 is initialized as constant for test code */
static table1_t table1[] = {
	{ /* .name = */ "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 16, { 0x0, } }, /* 1 */
	{ /* .name = */ "ronah",  /* .age = */127, { 0x0, } },
        { /* .name = */ "rlan",   /* .age = */118, { 0x0, } },
        { /* .name = */ "rlory",  /* .age = */128, { 0x0, } },
        { /* .name = */ "ropeye", /* .age = */118, { 0x0, } },
        { /* .name = */ "rlan",   /* .age = */128, { 0x0, } },
        { /* .name = */ "rlan",   /* .age = */138, { 0x0, } },
        { /* .name = */ "rlan",   /* .age = */148, { 0x0, } },
        { /* .name = */ "rlan",   /* .age = */158, { 0x0, } },
        { /* .name = */ "rdam",   /* .age = */168, { 0x0, } },
        { /* .name = */ "rnton",  /* .age = */123, { 0x0, } },
        { /* .name = */ "rnton",  /* .age = */124, { 0x0, } },
        { /* .name = */ "rieter", /* .age = */125, { 0x0, } },
        { /* .name = */ "roerg",  /* .age = */126, { 0x0, } },
        { /* .name = */ "rhomas", /* .age = */122, { 0x0, } },
        { /* .name = */ "rrank",  /* .age = */120, { 0x0, } },
        { /* .name = */ "Bruno" , /* .age = */112, { 0x0, } },
        { /* .name = */ "rlumi" , /* .age = */115, { 0x0, } },
        { /* .name = */ "rikey",  /* .age = */115, { 0x0, } },
        { /* .name = */ "rlong",  /* .age = */114, { 0x0, } },
        { /* .name = */ "riffy",  /* .age = */113, { 0x0, } },
        { /* .name = */ "riffy",  /* .age = */112, { 0x0, } }, /* 22 */
};

/*
 * Decouple the entries to maintain the multihash table from the data
 * in table1, since we do not want to transfer empty entries over the
 * PCIe bus to the card.
 */
static table2_t table2[] = {
        { /* .name = */ "ronah", /* .animal = */ "Whales"   }, /* 1 */
        { /* .name = */ "ronah", /* .animal = */ "Spiders"  },
        { /* .name = */ "rlan",  /* .animal = */ "Ghosts"   },
        { /* .name = */ "rlan",  /* .animal = */ "Zombies"  },
        { /* .name = */ "rlory", /* .animal = */ "Buffy"    },
        { /* .name = */ "rrobi", /* .animal = */ "Giraffe"  },
        { /* .name = */ "roofy", /* .animal = */ "Lion"     },
        { /* .name = */ "rumie", /* .animal = */ "Gepard"   },
        { /* .name = */ "rlumi", /* .animal = */ "Cow"      },
        { /* .name = */ "roofy", /* .animal = */ "Ape"      },
        { /* .name = */ "roofy", /* .animal = */ "Fish"     },
        { /* .name = */ "rikey", /* .animal = */ "Trout"    },
        { /* .name = */ "rikey", /* .animal = */ "Greyling" },
        { /* .name = */ "rnton", /* .animal = */ "Eagle"    },
        { /* .name = */ "rhomy", /* .animal = */ "Austrich" },
        { /* .name = */ "rlomy", /* .animal = */ "Sharks"   },
        { /* .name = */ "rroof", /* .animal = */ "Fly"      },
        { /* .name = */ "rlimb", /* .animal = */ "Birds"    },
        { /* .name = */ "rlong", /* .animal = */ "Buffy"    },
        { /* .name = */ "rrank", /* .animal = */ "Turtles"  },
        { /* .name = */ "rrank", /* .animal = */ "Gorillas" },
        { /* .name = */ "roffy", /* .animal = */ "Buffy"    },
        { /* .name = */ "ruffy", /* .animal = */ "Buffy"    },
        { /* .name = */ "rrank", /* .animal = */ "Buffy"    },
        { /* .name = */ "Bruno", /* .animal = */ "Buffy"    },
	{ "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "ABCDEFGHIJKLMNOPQRSTUVWXYZ" }, /* 26 */

};

#define TABLE2_N 8

/* worst case size */
static table3_t table3[TABLE1_SIZE * TABLE2_SIZE * TABLE2_N];

int main(void)
{
	unsigned int i;
	unsigned int table2_entries = 0;
	unsigned int t3_found;
	unsigned int table3_found = 0;
	snap_membus_t din_gmem[2048];    /* content is here */
	snap_membus_t dout_gmem[2048];   /* output goes here, empty */
	snap_membus_t d_ddrmem[2048];    /* card memory is empty */
	action_input_reg Action_Input;
	action_output_reg Action_Output;

	memset(din_gmem,  0, sizeof(din_gmem));
	memset(dout_gmem, 0, sizeof(dout_gmem));
	memset(d_ddrmem,  0, sizeof(d_ddrmem));

	Action_Input.Control.action = HASHJOIN_ACTION_TYPE;

	Action_Input.Data.t1.type = HOST_DRAM;
	Action_Input.Data.t1.address = 0;
	Action_Input.Data.t1.size = sizeof(table1);

	table2_entries = ARRAY_SIZE(table2) * TABLE2_N;
	Action_Input.Data.t2.type = HOST_DRAM;
	Action_Input.Data.t2.address = sizeof(table1);

	Action_Input.Data.t3.type = HOST_DRAM;
	Action_Input.Data.t3.address = sizeof(table1) + TABLE2_N * sizeof(table2);
	Action_Input.Data.t3.size = sizeof(table3);

	memcpy((uint8_t *)din_gmem, table1, sizeof(table1));

	/* Create a copy of table2 TABLE2_N times */
	for (i = 0; i < TABLE2_N; i++) {
		memcpy((uint8_t *)din_gmem + sizeof(table1) + i * sizeof(table2),
		       table2, sizeof(table2));
	}

#if defined(CONFIG_MEM_DEBUG)
	printf("HOSTMEMORY INPUT %p\n", din_gmem);
	for (unsigned int i = 0; i < 2048; i++)
		if (din_gmem[i] != 0)
			cout << setw(4)  << i * sizeof(snap_membus_t) << ": "
			     << setw(32) << hex << din_gmem[i]
			     << endl;
#endif
	i = 0;
	while (table2_entries != 0) {
		unsigned int t3_data;
		unsigned int todo = MIN(table2_entries, TABLE2_SIZE);

		Action_Input.Data.t2.size = todo * sizeof(table2_t);
		
		fprintf(stderr, "Processing %d table2 entries ...\n", todo);
		action_wrapper(din_gmem, dout_gmem, d_ddrmem,
			       &Action_Input, &Action_Output);

		Action_Input.Data.t1.address = 0; /* no need to process t1 */
		Action_Input.Data.t1.size = 0;
		Action_Input.Data.t2.address += todo * sizeof(table2_t);

		t3_found = (int)Action_Output.Data.t3_produced;
		t3_data = t3_found * sizeof(table3_t);
		Action_Input.Data.t3.address += t3_data;
		Action_Input.Data.t3.size -= t3_data;

		table3_found += t3_found;
		table2_entries -= todo;
		i++;
	}

#if defined(CONFIG_MEM_DEBUG)
	printf("HOSTMEMORY OUTPUT %p\n", dout_gmem);
	for (unsigned int i = 0; i < 2048; i++)
		if (dout_gmem[i] != 0)
			cout << setw(4)  << i * sizeof(snap_membus_t) << ": "
			     << setw(32) << hex << dout_gmem[i]
			     << endl;
#endif
	
	/* The 24 entries are a manually determined value */
	printf("Number of entries in t3: %d\n", table3_found);
	if (table3_found != 24 * TABLE2_N) {
		table3_dump((table3_t *)((uint8_t *)dout_gmem +
					 sizeof(table1) + TABLE2_N * sizeof(table2)),
			    table3_found);
		return 1;
	}

        return 0;
}

#endif /* NO_SYNTH */
