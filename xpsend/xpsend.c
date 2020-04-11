#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/ppdev.h>
#include <linux/parport.h>

#define LPT_DEV "/dev/parport0"

#define EXCHANGE_LEN	1048576

int lpt_fd,action;
unsigned int upload_addr;
const char *lpt_dev = LPT_DEV;
const char *send_file = NULL;

void pport_clear()
{
	struct ppdev_frob_struct frob;
	int val = 0;
	
	frob.mask	= PARPORT_CONTROL_SELECT;
	frob.val	= PARPORT_CONTROL_SELECT;
	
	ioctl(lpt_fd, PPWDATA, &frob.val);
	ioctl(lpt_fd, PPFCONTROL, &val);
	
	usleep(1000);
}


int pport_send(int byte)
{	
	struct ppdev_frob_struct frob;
	int timeout,status;
	
	// Send the data and put select high
	frob.mask	= PARPORT_CONTROL_SELECT;
	frob.val	= 0;
	ioctl(lpt_fd, PPWDATA, &byte);
	ioctl(lpt_fd, PPFCONTROL, &frob);
	
	// Wait until ack signal goes high
	status = 0;
	timeout = 0;
	do
	{
		if( timeout > 10000 )
		{
			frob.val = PARPORT_CONTROL_SELECT;
			ioctl(lpt_fd, PPFCONTROL, &frob);
			return 1;
		}
		//usleep(5);
		ioctl(lpt_fd, PPRSTATUS, &status);
		timeout++;
	} while(!(status & PARPORT_STATUS_ACK));
	
	// Turn off select line
	frob.val	= PARPORT_CONTROL_SELECT;
	ioctl(lpt_fd, PPFCONTROL, &frob);
	
	// Wait until ack signal goes low
	status = PARPORT_STATUS_ACK;
	timeout = 0;
	do
	{
		if( timeout > 10000 )
		{
			frob.val = PARPORT_CONTROL_SELECT;
			ioctl(lpt_fd, PPFCONTROL, &frob);
			return 1;
		}
		//usleep(5);
		ioctl(lpt_fd, PPRSTATUS, &status);
		timeout++;
	} while((status & PARPORT_STATUS_ACK));
	
	frob.val	= 0;
	ioctl(lpt_fd, PPRSTATUS, &status);
	
	return 0;
}

int pport_sendbytes(unsigned char *bytes, int len)
{
	int i;
	
	for(i=0; i<len; i++)
	{
		if( pport_send(bytes[i]) )
		{
			return 1;
		}
	}
	return 0;
}


#define CRC32_REMAINDER		0xFFFFFFFF

void initTable32(unsigned int* table) {

	int i,j;
	unsigned int crcVal;

	for(i=0; i<256; i++) {

		crcVal = i;

		for(j=0; j<8; j++) {

			if (crcVal&0x00000001L)
				crcVal = (crcVal>>1)^0xEDB88320L;
			else
				crcVal = crcVal>>1;

		}

		table[i] = crcVal;

	}

}

unsigned int crc32(void* buff, int bytes, unsigned int crc) {

	int	i;
	unsigned char*	byteBuff = (unsigned char*)buff;
	unsigned int	byte;
	unsigned int	crcTable[256];

    initTable32(crcTable);

	for(i=0; i<bytes; i++) {

		byte = 0x000000ffL&(unsigned int)byteBuff[i];
		crc = (crc>>8)^crcTable[(crc^byte)&0xff];

	}

	return(crc^0xFFFFFFFF);

}


#define	MAX_prg_entry_count	128

#pragma pack(push, 1)

typedef struct {

	unsigned int magic;				// 0-3
	unsigned char word_size;		// 4
	unsigned char endianness;		// 5
	unsigned char elf_version;		// 6
	unsigned char os_abi;			// 7
	unsigned int unused[2];			// 8-15

	unsigned short type;			// 16-17
	unsigned short instr_set;		// 18-19
	unsigned int elf_version2;		// 20-23

	unsigned int prg_entry_addr;	// 24-27
	unsigned int prg_head_pos;		// 28-31
	unsigned int sec_head_pos;		// 32-35
	unsigned int flags;				// 36-39
	unsigned short head_size;		// 40-41
	unsigned short prg_entry_size;	// 42-23
	unsigned short prg_entry_count;	// 44-45
	unsigned short sec_entry_size;	// 46-47
	unsigned short sec_entry_count;	// 48-49
	unsigned short sec_names_index;	// 50-51

} ELF_HEADER;

typedef struct {
	unsigned int seg_type;
	unsigned int p_offset;
	unsigned int p_vaddr;
	unsigned int undefined;
	unsigned int p_filesz;
	unsigned int p_memsz;
	unsigned int flags;
	unsigned int alignment;
} PRG_HEADER;

#pragma pack(pop)

typedef struct {
	unsigned int pc0;
	unsigned int gp0;
	unsigned int t_addr;
	unsigned int t_size;
	unsigned int d_addr;
	unsigned int d_size;
	unsigned int b_addr;
	unsigned int b_size;
	unsigned int sp_addr;
	unsigned int sp_size;
	unsigned int sp;
	unsigned int fp;
	unsigned int gp;
	unsigned int ret;
	unsigned int base;
} EXEC;

typedef struct {
	char header[8];
	char pad[8];
	EXEC params;
	char license[64];
	char pad2[1908];
} PSEXE;

typedef struct {
	EXEC params;
	unsigned int crc32;
	unsigned int flags;
} EXEPARAM;

typedef struct {
    int size;
    unsigned int addr;
    unsigned int crc32;
} BINPARAM;


void* loadELF(FILE* fp, EXEC* param) {

	ELF_HEADER head;
	PRG_HEADER prg_heads[MAX_prg_entry_count];
	unsigned int exe_taddr = 0xffffffff;
	unsigned int exe_haddr = 0;
	unsigned int exe_tsize = 0;
	unsigned char* binary;
	
	fseek(fp, 0, SEEK_SET);
	fread(&head, 1, sizeof(head), fp);

	// Check header
	if( head.magic != 0x464c457f ) {
		
		printf("File is neither a PS-EXE, CPE or ELF binary.\n");
		return NULL;
		
	}

	if( head.type != 2 ) {
		
		printf("Only executable ELF files are supported.\n");
		return NULL;
		
	}

	if( head.instr_set != 8 ) {
		
		printf("ELF file is not a MIPS binary.\n");
		return NULL;
		
	}

	if( head.word_size != 1 ) {
		
		printf("Only 32-bit ELF files are supported.\n");
		return NULL;
		
	}

	if( head.endianness != 1 ) {
		
		printf("Only little endian ELF files are supported.\n");
		return NULL;
		
	}


	// Load program headers and determine binary size and load address

	fseek(fp, head.prg_head_pos, SEEK_SET);
	for( int i=0; i<head.prg_entry_count; i++ ) {

		fread( &prg_heads[i], 1, sizeof(PRG_HEADER), fp );

		if( prg_heads[i].flags == 4 ) {
			continue;
		}

		if( prg_heads[i].p_vaddr < exe_taddr ) {
			exe_taddr = prg_heads[i].p_vaddr;
		}

		if( prg_heads[i].p_vaddr > exe_haddr ) {
			exe_haddr = prg_heads[i].p_vaddr;
		}

	}

	exe_tsize = (exe_haddr-exe_taddr);
	exe_tsize += prg_heads[head.prg_entry_count-1].p_filesz;
	

	// Check if load address is appropriate in main RAM locations
	if( ( ( exe_taddr>>24 ) == 0x0 ) || ( ( exe_taddr>>24 ) == 0x80 ) ||
		( ( exe_taddr>>24 ) == 0xA0 ) ) {

		if( ( exe_taddr&0x00ffffff ) < 65536 ) {

			printf( "WARNING: Program text address overlaps kernel area!\n" );

		}

	}


	// Pad out the size to multiples of 2KB
	exe_tsize = 2048*((exe_tsize+2047)/2048);

	// Load the binary data
	binary = (unsigned char*)malloc( exe_tsize );
	memset( binary, 0x0, exe_tsize );

	for( int i=0; i<head.prg_entry_count; i++ ) {

		if( prg_heads[i].flags == 4 ) {
			continue;
		}

		fseek( fp, prg_heads[i].p_offset, SEEK_SET );
		fread( &binary[(int)(prg_heads[i].p_vaddr-exe_taddr)],
			1, prg_heads[i].p_filesz, fp );

	}
	
	memset(param, 0, sizeof(EXEC));
	param->pc0 = head.prg_entry_addr;
	param->t_addr = exe_taddr;
	param->t_size = exe_tsize;
	
	return binary;
	
}

void* loadCPE(FILE* fp, EXEC* param) {
	
	int val, exe_size = 0;
	unsigned int uv, exe_entry = 0;
	unsigned int *addr_list = NULL;
	int addr_list_len = 0;	
	
	// Check CPE magic word
	fseek(fp, 0, SEEK_SET);
	fread(&val, 1, 4, fp);
	
	if( val != 0x01455043 )
	{
		return NULL;
	}
	
	// Clear EXEC parameters
	memset(param, 0x0, sizeof(EXEC));
	
	// Begin parsing CPE data
	val = 0;
	fread(&val, 1, 1, fp);
	
	while( val )
	{
		switch( val )
		{
		case 0x1:	// Binary chunk
				
			fread(&uv, 1, 4, fp);
			fread(&val, 1, 4, fp);
				
			addr_list_len++;
			addr_list = (unsigned int*)realloc(addr_list, 
				sizeof(unsigned int)*addr_list_len);
				
			addr_list[addr_list_len-1] = uv;
			exe_size += val;
				
			fseek(fp, val, SEEK_CUR);
			break;
				
		case 0x3:	// Set register, ignored
				
			val = 0;
			fread(&val, 1, 2, fp);
				
			if( val != 0x90 )
			{
				printf("Warning: Unknown SETREG code: %d\n", val);
			}
				
			fread(&exe_entry, 1, 4, fp);
			break;
				
		case 0x8:	// Select unit, ignored
				
			val = 0;
			fread(&val, 1, 1, fp);
			break;
				
		default:
			
			printf("Unknown chunk found: %d\n", val);
			free(addr_list);
			return NULL;
				
		}
		
		// Read next code
		val = 0;
		fread(&val, 1, 1, fp);
		
	}
	
	
	// Begin loading the executable data
	char* exe_buff;
	
	unsigned int addr_upper=0;
	unsigned int addr_lower=0;
	
	// Find the highest chunk address
	for(int i=0; i<addr_list_len; i++) {
		
		if( addr_list[i] > addr_upper ) {
			
			addr_upper = addr_list[i];
			
		}
		
	}
	
	// Find the lowest chunk address
	addr_lower = addr_upper;
	
	for(int i=0; i<addr_list_len; i++) {
		
		if ( addr_list[i] < addr_lower ) {
			
			addr_lower = addr_list[i];
			
		}
		
	}
	
	// Pad executable size to multiples of 2KB and allocate
	exe_size = 2048*((exe_size+2047)/2048);
	
	exe_buff = (char*)malloc(exe_size);
	memset(exe_buff, 0x0, exe_size);
	
	// Load binary chunks
	val = 0;
	fseek(fp, 4, SEEK_SET);
	fread(&val, 1, 1, fp);
	
	while( val ) {
		
		switch( val ) {
			case 0x1:	// Binary chunk
				
				fread(&uv, 1, 4, fp);
				fread(&val, 1, 4, fp);
				
				fread(exe_buff+(uv-addr_lower), 1, val, fp);
				
				break;
				
			case 0x3:	// Set register (skipped)
				
				val = 0;
				fread(&val, 1, 2, fp);
				
				if( val == 0x90 ) {
					
					fseek(fp, 4, SEEK_CUR);
					
				}
				
				break;
				
			case 0x8:	// Select unit (skipped)
				
				fseek(fp, 1, SEEK_CUR);
				
				break;
		}
		
		val = 0;
		fread(&val, 1, 1, fp);
		
	}
	
	// Set program text and entrypoint
	param->pc0 = exe_entry;
	param->t_addr = addr_lower;
	param->t_size = exe_size;
	
	return exe_buff;
	
}

int uploadEXE(const char* exefile) {
	
	PSEXE exe;
	EXEPARAM param;
	int i,progress,last_progress;
	
	unsigned char* buffer;
	unsigned int pos;
	
	struct timeval otv;
	struct timeval ntv;
	float dtime;
	
	FILE* fp = fopen(exefile, "rb");
	
	if ( fp == NULL ) {
	
		printf("File not found.\n");
		return -1;
		
	}
	
	// Load PS-EXE header
	if( fread(&exe, 1, sizeof(PSEXE), fp) != sizeof(PSEXE) ) {
		
		printf("Read error or invalid file.\n");
		fclose(fp);
		return -1;
		
	}
	
	// Check file ID
	if( strcmp(exe.header, "PS-X EXE") ) {
		
		// If not PS-EXE, try loading it as CPE
		buffer = (unsigned char*)loadCPE(fp, &param.params);
		
		if( buffer == NULL ) {
			
			// If not CPE, try loading it as ELF
			buffer = (unsigned char*)loadELF(fp, &param.params);
			
			if( buffer == NULL ) {
			
				fclose(fp);
				return -1;
				
			}
			
		}
		
	} else {
		
		// Load PS-EXE body, simple
		buffer = (unsigned char*)malloc(exe.params.t_size);
	
		if( fread(buffer, 1, exe.params.t_size, fp) != exe.params.t_size ) {
			
			printf("Incomplete file or read error.\n");
			free(buffer);
			fclose(fp);
			
			return -1;
			
		}
		
		memcpy(&param.params, &exe.params, sizeof(EXEPARAM));
		
	}
	
	// Done with the file
	fclose(fp);
	
	// Calculate CRC32 of binary data
	param.crc32 = crc32(buffer, param.params.t_size, CRC32_REMAINDER);
	param.flags = 0;
	
	
	// Commence upload
	if( pport_send('E') )
	{
		printf("Target not responding.\n");
		return 1;
	}
	
	if( pport_sendbytes((unsigned char*)&param, sizeof(EXEPARAM)) )
	{
		printf("Error sending PS-EXE parameters.\n");
		return 1;
	}
	
	gettimeofday(&otv, NULL);
	
	printf("Uploading executable file...\n");
	printf("[");
	for(i=0; i<50; i++)
	{
		printf(".");
	}
	printf("]\r\e[1C");
	fflush(stdout);
	
	last_progress = 0;
	for(pos=0; pos<param.params.t_size; pos++)
	{
		progress = (51*((1024*(pos>>2))/(param.params.t_size>>2)))/1024;
		if( progress > last_progress )
		{
			printf("#");
			fflush(stdout);
			last_progress = progress;
		}
		if( pport_send(buffer[pos]) )
		{
			printf("\nTimeout error.\n");
			free(buffer);
			return 1;
		}
	}
	
	printf("\n");
	
	gettimeofday(&ntv, NULL);
	
	dtime = (float)(ntv.tv_sec-otv.tv_sec);
	dtime += (float)((ntv.tv_usec-otv.tv_usec)/1000000.f);
	
	printf("Transfer of %d bytes took %.4fs (%.4fKB/s)\n", 
		param.params.t_size, dtime,
		((param.params.t_size/dtime))/1024.f);
		
	free(buffer);
	
	return 0;
}

int uploadBIN(const char *binfile, unsigned int addr)
{
	int i;
	int progress,last_progress;
	char *buffer;
	BINPARAM param;
	unsigned int total_upload,pos;
	
	struct timeval otv;
	struct timeval ntv;
	float dtime;
	
	// Open file
	FILE* fp = fopen(binfile, "rb");
	
	if( fp == NULL )
	{
		printf("Binary file not found.\n");
		return 1;
	}
	
	fseek(fp, 0, SEEK_END);
	param.size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	
	buffer = (char*)malloc(param.size);
	
	if( fread(buffer, 1, param.size, fp) != param.size )
	{
		printf("Error reading file.");
		fclose(fp);
		free(buffer);
		return 1;
	}
	
	fclose(fp);
	
	// Compute checksum of entire file
	param.addr = addr;
	param.crc32 = crc32(buffer, param.size, CRC32_REMAINDER);
		
	// Send binary upload command
	if( pport_send('B') )
	{
		printf("Target not responding.\n");
		free(buffer);
		return 1;
	}
	
	// Send binary parameters
	if( pport_sendbytes((unsigned char*)&param, sizeof(BINPARAM)) )
	{
		printf("Error sending binary parameters.\n");
		free(buffer);
		return 1;
	}
	
	// Get current time for measuring upload duration after
	gettimeofday(&otv, NULL);
	
	// Print out a progress bar
	printf("Uploading binary file to address 0x%x...\n", addr);
	printf("[");
	for(i=0; i<50; i++)
	{
		printf(".");
	}
	printf("]\r\e[1C");
	fflush(stdout);
	
	// Upload the binary data
	last_progress = 0;
	total_upload = 0;
	
	for(pos=0; pos<param.size; pos++)
	{
		progress = (51*((1024*(pos>>2))/(param.size>>2)))/1024;
		if( progress > last_progress )
		{
			printf("#");
			fflush(stdout);
			last_progress = progress;
		}
		if( pport_send(buffer[pos]) )
		{
			printf("\nTimeout error.\n");
			free(buffer);
			return 1;
		}
	}
			
	printf("\n");
	
	gettimeofday(&ntv, NULL);
	
	dtime = (float)(ntv.tv_sec-otv.tv_sec);
	dtime += (float)((ntv.tv_usec-otv.tv_usec)/1000000.f);
	
	printf("Transfer of %d bytes took %.4fs (%.4fKB/s)\n", 
		param.size, dtime,
		((param.size/dtime))/1024.f);
	
	return 0;
}

int main(int argc, const char *argv[])
{
	int i,j;
	
	printf("XPSEND - Xplorer upload tool for n00bROM\n");
	printf("2020 Lameguy64 / Meido-Tek Productions\n\n");
		
	if( argc <= 1 )
	{
		printf("xpsend [-d <dev>] <run|up> <file> [addr]\n\n");
		printf("Arguments:\n");
		printf("  -d <dev>         - Specify parallel port device to use.\n");
		printf("  run <exefile>    - Upload a PS-EXE/CPE or ELF executable.\n");
		printf("  up <file> <addr> - Upload a binary file to the specified address.\n\n");
		
		return 0;
	}
	
	action = 0;
	for( i=1; i<argc; i++ )
	{
		if( argv[i][0] == '-' )
		{
			if( strcasestr(argv[i], "-d") == 0 )
			{
				i++;
				if( i >= argc )
				{
					printf("No device specified.\n");
					return EXIT_FAILURE;
				}
				lpt_dev = argv[i];
			}
		}
		else if( strcmp(argv[i], "run") == 0 )
		{
			if( action == 0 )
			{
				i++;
				if( i >= argc )
				{
					printf("No file specified.\n");
					return EXIT_FAILURE;
				}
				send_file = argv[i];
				action = 1;
			}
		}
		else if( strcmp(argv[i], "up") == 0 )
		{
			if( action == 0 )
			{
				i++;
				if( i >= argc )
				{
					printf("No file specified.\n");
					return EXIT_FAILURE;
				}
				send_file = argv[i];
				i++;
				if( i >= argc )
				{
					printf("No address specified.\n");
					return EXIT_FAILURE;
				}
				sscanf(argv[i], "%x", &upload_addr);
				action = 2;
			}
		}
	}
	
	if( !action )
	{
		printf("No action was specified.\n");
		return EXIT_FAILURE;
	}
	
	// Open parallel port
	lpt_fd = open(lpt_dev, O_RDWR);
	if( lpt_fd < 0 )
	{
		printf("Unable to open " LPT_DEV "\n");
		return EXIT_FAILURE;
	}
	
	i = ioctl(lpt_fd, PPCLAIM);
	if( i < 0 )
	{
		printf("Unable to claim parallel port or invalid device.\n");
		close(lpt_fd);
		return EXIT_FAILURE;
	}
	
	/*
	i = ioctl(lpt_fd, PPGETMODES, &j);
	if( i < 0 )
	{
		printf("Unable to get available port modes.\n");
		ioctl(lpt_fd, PPRELEASE);
		close(lpt_fd);
		return EXIT_FAILURE;
	}
	
	printf("Supported modes: ");
	
	if( j&PARPORT_MODE_PCSPP )
		printf("SPP ");
	if( j&PARPORT_MODE_TRISTATE )
		printf("Bi-Directional ");
	if( j&PARPORT_MODE_EPP )
		printf("EPP ");
	if( j&PARPORT_MODE_ECP )
		printf("ECP ");
	if( j&PARPORT_MODE_COMPAT )
		printf("Compatibility ");
	if( j&PARPORT_MODE_DMA )
		printf("DMA ");
	printf("\n");
	*/
	
	pport_clear();
	
	
	if( action == 1 )
	{
		uploadEXE(send_file);
	}
	else if( action == 2 )
	{
		uploadBIN(send_file, upload_addr);
	}
	
	close( lpt_fd );
	
	
	return 0;
}
