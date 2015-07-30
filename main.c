#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <lzma.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>

#define MAX_COMPRESS_THREAD 8

// global config
///////////////////////////////////////////////////
const char * MY_ZIP_DATA_OFFSET = "MY_ZIP_DATA_OFFSET:18446744073709551616";
#define MY_ZIP_DATA_OFFSET_LEN 39
#define MY_ZIP_DATA_OFFSET_HEADER_LEN  19
const char * MY_ZIP_MODE = "MY_ZIP_MODE:0";
#define MY_ZIP_MODE_LEN  13
#define MY_ZIP_MODE_HEADER_LEN 12

int32_t operation_mode = 0; // 0: encode, 1: decode
uint32_t data_offset = 0;
int32_t verbose = 0;
int32_t thread_cnt = -1;
uint32_t compress_level = LZMA_PRESET_DEFAULT;
// global vars
///////////////////////////////////////////////////
uint64_t total_size = 0;
uint64_t current_size = 0;
//////////////////////////////////////////////////


static void
print_progress(uint32_t current_size, uint32_t total_size)
{
	double progress = 0.0;
	if(!verbose)
		return;
	progress = (100.0 * current_size) / total_size;
	fprintf(stderr, "\rIn progress %.2f%%", progress);
}

static int32_t
init_decoder(lzma_stream *strm, lzma_ret * lzma_err)
{
	*lzma_err = LZMA_OK;
	// Initialize a .xz decoder. The decoder supports a memory usage limit
	// and a set of flags.
	//
	// The memory usage of the decompressor depends on the settings used
	// to compress a .xz file. It can vary from less than a megabyte to
	// a few gigabytes, but in practice (at least for now) it rarely
	// exceeds 65 MiB because that's how much memory is required to
	// decompress files created with "xz -9". Settings requiring more
	// memory take extra effort to use and don't (at least for now)
	// provide significantly better compression in most cases.
	//
	// Memory usage limit is useful if it is important that the
	// decompressor won't consume gigabytes of memory. The need
	// for limiting depends on the application. In this example,
	// no memory usage limiting is used. This is done by setting
	// the limit to UINT64_MAX.
	//
	// The .xz format allows concatenating compressed files as is:
	//
	//     echo foo | xz > foobar.xz
	//     echo bar | xz >> foobar.xz
	//
	// When decompressing normal standalone .xz files, LZMA_CONCATENATED
	// should always be used to support decompression of concatenated
	// .xz files. If LZMA_CONCATENATED isn't used, the decoder will stop
	// after the first .xz stream. This can be useful when .xz data has
	// been embedded inside another file format.
	//
	// Flags other than LZMA_CONCATENATED are supported too, and can
	// be combined with bitwise-or. See lzma/container.h
	// (src/liblzma/api/lzma/container.h in the source package or e.g.
	// /usr/include/lzma/container.h depending on the install prefix)
	// for details.
	lzma_ret ret = lzma_stream_decoder(
			strm, UINT64_MAX, LZMA_CONCATENATED);

	// Return successfully if the initialization went fine.
	if (ret == LZMA_OK)
		return 0;

	// Something went wrong. The possible errors are documented in
	// lzma/container.h (src/liblzma/api/lzma/container.h in the source
	// package or e.g. /usr/include/lzma/container.h depending on the
	// install prefix).
	//
	// Note that LZMA_MEMLIMIT_ERROR is never possible here. If you
	// specify a very tiny limit, the error will be delayed until
	// the first headers have been parsed by a call to lzma_code().
	*lzma_err = ret;
	return 1;
}


static int32_t
decompress(lzma_stream *strm, FILE *infile, FILE *outfile,
	lzma_ret * lzma_err, int32_t * filein_err, int32_t * fileout_err)
{
	*lzma_err = LZMA_OK;
	*filein_err = 0;
	*fileout_err = 0;
	// When LZMA_CONCATENATED flag was used when initializing the decoder,
	// we need to tell lzma_code() when there will be no more input.
	// This is done by setting action to LZMA_FINISH instead of LZMA_RUN
	// in the same way as it is done when encoding.
	//
	// When LZMA_CONCATENATED isn't used, there is no need to use
	// LZMA_FINISH to tell when all the input has been read, but it
	// is still OK to use it if you want. When LZMA_CONCATENATED isn't
	// used, the decoder will stop after the first .xz stream. In that
	// case some unused data may be left in strm->next_in.
	lzma_action action = LZMA_RUN;

	uint8_t inbuf[BUFSIZ];
	uint8_t outbuf[BUFSIZ];

	strm->next_in = NULL;
	strm->avail_in = 0;
	strm->next_out = outbuf;
	strm->avail_out = sizeof(outbuf);

	while (true) {
		if (strm->avail_in == 0 && !feof(infile)) {
			strm->next_in = inbuf;
			strm->avail_in = fread(inbuf, 1, sizeof(inbuf),
					infile);

			current_size += strm->avail_in;
			print_progress(current_size, total_size);

			if (ferror(infile)) {
				*filein_err = errno;
				return 2;
			}

			// Once the end of the input file has been reached,
			// we need to tell lzma_code() that no more input
			// will be coming. As said before, this isn't required
			// if the LZMA_CONATENATED flag isn't used when
			// initializing the decoder.
			if (feof(infile))
				action = LZMA_FINISH;
		}

		lzma_ret ret = lzma_code(strm, action);

		if (strm->avail_out == 0 || ret == LZMA_STREAM_END) {
			size_t write_size = sizeof(outbuf) - strm->avail_out;

			if (fwrite(outbuf, 1, write_size, outfile)
					!= write_size) {
				*fileout_err = errno;
				return 3;
			}

			strm->next_out = outbuf;
			strm->avail_out = sizeof(outbuf);
		}

		if (ret != LZMA_OK) {
			// Once everything has been decoded successfully, the
			// return value of lzma_code() will be LZMA_STREAM_END.
			//
			// It is important to check for LZMA_STREAM_END. Do not
			// assume that getting ret != LZMA_OK would mean that
			// everything has gone well or that when you aren't
			// getting more output it must have successfully
			// decoded everything.
			if (ret == LZMA_STREAM_END)
				return 0;

			// It's not LZMA_OK nor LZMA_STREAM_END,
			// so it must be an error code. See lzma/base.h
			// (src/liblzma/api/lzma/base.h in the source package
			// or e.g. /usr/include/lzma/base.h depending on the
			// install prefix) for the list and documentation of
			// possible values. Many values listen in lzma_ret
			// enumeration aren't possible in this example, but
			// can be made possible by enabling memory usage limit
			// or adding flags to the decoder initialization.


			*lzma_err = ret;
			return 1;
		}
	}
}

static int32_t
init_encoder(lzma_stream *strm, lzma_ret * lzma_err)
{
	*lzma_err = LZMA_OK;
	// The threaded encoder takes the options as pointer to
	// a lzma_mt structure.
	lzma_mt mt = {
		// No flags are needed.
		.flags = 0,

		// Let liblzma determine a sane block size.
		.block_size = 0,

		// Use no timeout for lzma_code() calls by setting timeout
		// to zero. That is, sometimes lzma_code() might block for
		// a long time (from several seconds to even minutes).
		// If this is not OK, for example due to progress indicator
		// needing updates, specify a timeout in milliseconds here.
		// See the documentation of lzma_mt in lzma/container.h for
		// information how to choose a reasonable timeout.
		.timeout = 0,

		// Use the default preset (6) for LZMA2.
		// To use a preset, filters must be set to NULL.
		.preset = compress_level,
		.filters = NULL,

		// Use CRC64 for integrity checking. See also
		// 01_compress_easy.c about choosing the integrity check.
		.check = LZMA_CHECK_CRC64,
	};

	// Detect how many threads the CPU supports.
	mt.threads = lzma_cputhreads();

	// If the number of CPU cores/threads cannot be detected,
	// use one thread. Note that this isn't the same as the normal
	// single-threaded mode as this will still split the data into
	// blocks and use more RAM than the normal single-threaded mode.
	// You may want to consider using lzma_easy_encoder() or
	// lzma_stream_encoder() instead of lzma_stream_encoder_mt() if
	// lzma_cputhreads() returns 0 or 1.
	if (mt.threads == 0)
		mt.threads = 1;

	// If the number of CPU cores/threads exceeds threads_max,
	// limit the number of threads to keep memory usage lower.
	// The number 8 is arbitrarily chosen and may be too low or
	// high depending on the compression preset and the computer
	// being used.
	//
	// FIXME: A better way could be to check the amount of RAM
	// (or available RAM) and use lzma_stream_encoder_mt_memusage()
	// to determine if the number of threads should be reduced.
	if (mt.threads > thread_cnt)
		mt.threads = thread_cnt;

	// Initialize the threaded encoder.
	lzma_ret ret = lzma_stream_encoder_mt(strm, &mt);

	if (ret == LZMA_OK)
		return 0;

	*lzma_err = ret;

	return 1;
}

static int32_t
compress(lzma_stream *strm, FILE *infile, FILE *outfile,
	lzma_ret * lzma_err, int32_t * filein_err, int32_t * fileout_err)
{
	*lzma_err = LZMA_OK;
	*filein_err = 0;
	*fileout_err = 0;

	lzma_action action = LZMA_RUN;

	uint8_t inbuf[BUFSIZ];
	uint8_t outbuf[BUFSIZ];

	strm->next_in = NULL;
	strm->avail_in = 0;
	strm->next_out = outbuf;
	strm->avail_out = sizeof(outbuf);

	while (true) {
		if (strm->avail_in == 0 && !feof(infile)) {
			strm->next_in = inbuf;
			strm->avail_in = fread(inbuf, 1, sizeof(inbuf),
					infile);

			current_size += strm->avail_in;
			print_progress(current_size, total_size);

			if (ferror(infile)) {
				*filein_err = errno;
				return 2;
			}

			if (feof(infile))
				action = LZMA_FINISH;
		}

		lzma_ret ret = lzma_code(strm, action);

		if (strm->avail_out == 0 || ret == LZMA_STREAM_END) {
			size_t write_size = sizeof(outbuf) - strm->avail_out;

			if (fwrite(outbuf, 1, write_size, outfile)
					!= write_size) {
				*fileout_err = errno;
				return 3;
			}

			strm->next_out = outbuf;
			strm->avail_out = sizeof(outbuf);
		}

		if (ret != LZMA_OK) {
			if (ret == LZMA_STREAM_END)
				return 0;

			*lzma_err = ret;
			return 1;
		}
	}
}

static char * err_msg[] = {
	"Operation completed successfully",
	"End of stream was reached",
	"Input stream has no integrity check",
	"Cannot calculate the integrity check",
	"Integrity check type is now available",
	"Cannot allocate memory",
	"Memory usage limit was reached",
	"File format not recognized",
	"Invalid or unsupported options",
	"Data is corrupt",
	"No progress is possible",
	"Programming error"
};

static const char * 
lzma_strerror(lzma_ret code)
{
	return err_msg[(int32_t)code];
}

static void
load_mode()
{
	if(MY_ZIP_MODE[MY_ZIP_MODE_HEADER_LEN] == '0') {
		operation_mode = 0;
	} else {
		operation_mode = 1;
	}
}


static int32_t
get_file_size(FILE * file, uint64_t * size)
{
	struct stat st;
	if(fstat(fileno(file), &st) < 0) {
		fprintf(stderr, "get file size error: %s\n", strerror(errno));
		return 1;
	}
	*size = st.st_size;
	return 0;
}


static void *
my_memmem(const void *haystack, size_t haystacklen,
                    const void *needle, size_t needlelen)
{
	const char * p = haystack;
	if(needlelen > haystacklen) return NULL;

	while(1) {
		if(!memcmp(p, needle, needlelen)) 
			break;
		p ++;
		haystacklen --;
		if(haystacklen < needlelen) {
			p = NULL;
			break;
		}
	}
	
	return (void *)p;
}

static uint8_t * 
init_decompress_header(const char * file, uint32_t * len)
{
	FILE *infile = NULL;
	uint8_t * header = NULL;
	uint64_t file_size;
	char * p = NULL;

	infile = fopen(file, "rb");
	if (infile == NULL) {
		goto err;
	}

	if(get_file_size(infile, &file_size) < 0) {
		goto err;
	}

	if((header = malloc(file_size)) == NULL) {
		fprintf(stderr, "malloc error\n");
		goto err;
	}

	fread(header, 1, file_size, infile);
	if (ferror(infile)) {
		fprintf(stderr, "read file error: %s\n", strerror(errno));
		goto err;
	}

	p = my_memmem(header, file_size,
                    MY_ZIP_DATA_OFFSET,MY_ZIP_DATA_OFFSET_LEN);

	if(NULL == p) {
		fprintf(stderr, "corrupt file header\n");
		goto err;
	}

	p += MY_ZIP_DATA_OFFSET_HEADER_LEN;

	sprintf(p, "%u", file_size);

	p = my_memmem(header, file_size,
                    MY_ZIP_MODE,MY_ZIP_MODE_LEN);

	if(NULL == p) {
		fprintf(stderr, "corrupt file header\n");
		goto err;
	}

	p += MY_ZIP_MODE_HEADER_LEN;

	*p = '1';

	*len = file_size;

	return header;

err:
	if(NULL != header) {
		free(header);
	}
	return NULL;
}


static struct option compress_options[] = {
	{"level",   required_argument, 0,  'l' },
	{"extreme", no_argument,       0,  'e' },
	{"verbose", no_argument,       0,  'v' },
	{"thread",  required_argument, 0,  't' },
	{"help",    no_argument,       0,  'h' },
	{0,         0,                 0,  0   }
};
static const char * compress_option_desc[] = {
"compress level 0-9, default 6",
"exterme compression, default off",
"verbose mode, default off",
"max thread count, default 8",
"show help",
NULL
};

static void
compress_usage(const char *prog)
{
	int32_t i;
	fprintf(stderr, "%s: <OPTIONS> [input file] [output file]\n",prog);
	for(i = 0; i < sizeof(compress_options)/sizeof(struct option) - 1; i++) {
		fprintf(stderr, "    --%s|-%c: %s\n", compress_options[i].name, 
			compress_options[i].val, compress_option_desc[i]);
	}
}

static int32_t
compress_main(int32_t argc, char **argv)
{
	int32_t option_index = 0;
	int32_t opt;
	int32_t val;
    extern char *optarg;
    extern int optind, opterr, optopt;
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret lzma_err;
	int32_t filein_err, fileout_err;
	int32_t ret;
	FILE *outfile = NULL;
	FILE *infile = NULL;
	uint8_t * header = NULL;
	uint32_t header_len = 0;

	while((opt = getopt_long(argc, argv, "l:evth:",
		compress_options, &option_index)) != -1) {
		switch (opt) {
		case 'l':
			val = atoi(optarg);
			if(val < 0) val = 0;
			if(val > 9) val = 9;
			compress_level = val;
			break;
		case 'e':
			compress_level |= LZMA_PRESET_EXTREME;
			break;
		case 'v':
			verbose = 1;
			break;
		case 't':
			thread_cnt = atoi(optarg);
			if(thread_cnt > MAX_COMPRESS_THREAD) thread_cnt = MAX_COMPRESS_THREAD;
			if(thread_cnt < 0) thread_cnt = 1;
			break;
		case 'h':
			compress_usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		default: 
			compress_usage(argv[0]);
			exit(EXIT_FAILURE);
		}	
	}

	if (argc - optind != 2) {
		compress_usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if((header = init_decompress_header(argv[0], &header_len)) == NULL) {
		fprintf(stderr, "%s: Error init the header\n");
		goto err;
	}

	if(init_encoder(&strm, &lzma_err)!=0) {
		fprintf(stderr, "%s: Error init the encoder: %s\n",
					argv[0], lzma_strerror(lzma_err));
		goto err;
	}

	infile = fopen(argv[optind], "rb");
	if (infile == NULL) {
		fprintf(stderr, "%s: Error opening the input file: %s\n",
					argv[optind], strerror(errno));
		goto err;
	}

	if(get_file_size(infile, &total_size) < 0) {
		goto err;
	}

	outfile = fopen(argv[optind + 1], "wb");
	if (outfile == NULL) {
		fprintf(stderr, "%s: Error opening the output file: %s\n",
					argv[1], strerror(errno));
		goto err;
	}

	if (fwrite(header, 1, header_len, outfile) != header_len) {
		fprintf(stderr, "%s: Error write the output file: %s\n",
					argv[1], strerror(errno));
		goto err;
	}

	free(header);
	header = NULL;

	ret = compress(&strm, infile, outfile, &lzma_err, &filein_err, &fileout_err);

	fprintf(stderr, "\n");
	if(ret != 0) {
		if(ret == 1) {
			fprintf(stderr, "%s: Error compress the file: %s\n",
						argv[0], lzma_strerror(lzma_err));
		} else if(ret == 2) {
			fprintf(stderr, "%s: Error read the input file: %s\n",
						argv[0], strerror(filein_err));			
		} else {
			fprintf(stderr, "%s: Error write the output file: %s\n",
						argv[0], strerror(fileout_err));	
		}
		goto err;
	}

	lzma_end(&strm);

	if (fclose(infile)) {
		fprintf(stderr, "%s: Read error: %s\n", argv[optind], strerror(errno));
		goto err;
	}
	infile = NULL;

	if (fclose(outfile)) {
		fprintf(stderr, "%s: Write error: %s\n", argv[optind + 1], strerror(errno));
		goto err;
	}
	outfile = NULL;
	return EXIT_SUCCESS;
err:
	if(NULL != header) {
		free(header);
	}
	if(NULL != infile) {
		fclose(infile);
	}
	if(NULL != outfile) {
		fclose(outfile);
	}
	lzma_end(&strm);

	return EXIT_FAILURE;
}

static int32_t
load_offset()
{
	uint64_t val;
	char * endptr = NULL;
	val = strtoul(MY_ZIP_DATA_OFFSET + MY_ZIP_DATA_OFFSET_HEADER_LEN, &endptr, 10);
	if(val == ULONG_MAX && errno == ERANGE) {
		fprintf(stderr, "corrupt file header\n");
		return 1;
	}
	data_offset = val;
	return 0;
}

static struct option decompress_options[] = {
	{"verbose", no_argument, 0,  'v' },
	{"help", no_argument,  0, 'h' },
	{0, 0, 0,  0}
};
static const char * decompress_option_desc[] = {
"verbose mode, default off",
"show help",
NULL
};

static void
decompress_usage(const char *prog)
{
	int32_t i;
	fprintf(stderr, "%s: <OPTIONS> [output file]\n",prog);
	for(i = 0; i < sizeof(decompress_options)/sizeof(struct option) - 1; i++) {
		fprintf(stderr, "    --%s|-%c: %s\n", decompress_options[i].name, 
			decompress_options[i].val, decompress_option_desc[i]);
	}
}

static int32_t
decompress_main(int32_t argc, char **argv)
{
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret lzma_err;
	int32_t filein_err, fileout_err;
	int32_t ret;
	FILE *outfile = NULL;
	FILE *infile = NULL;
	int32_t option_index = 0;
	int32_t opt;
	int32_t val;
    extern char *optarg;
    extern int optind, opterr, optopt;

	while((opt = getopt_long(argc, argv, "vh",
		compress_options, &option_index)) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'h':
			decompress_usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		default: 
			decompress_usage(argv[0]);
			exit(EXIT_FAILURE);
		}	
	}

	if (argc - optind != 1) {
		decompress_usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (init_decoder(&strm, &lzma_err) != 0) {
		fprintf(stderr, "%s: Error init the decoder: %s\n",
					argv[0], lzma_strerror(lzma_err));
		goto err;
	}

	infile = fopen(argv[0], "rb");
	if (infile == NULL) {
		fprintf(stderr, "%s: Error opening the input file: %s\n",
					argv[0], strerror(errno));
		goto err;
	}

	if(load_offset() != 0) {
		fprintf(stderr, "%s: Error read the offset from input file\n",
					argv[0]);
		goto err;		
	}

	if(get_file_size(infile, &total_size) < 0) {
		goto err;
	}

	total_size -= data_offset;

	if(fseek(infile,data_offset,SEEK_SET) < 0) {
		fprintf(stderr, "%s: Error seeking the input file: %s\n",
					argv[0], strerror(errno));
	}

	outfile = fopen(argv[optind], "wb");
	if (outfile == NULL) {
		fprintf(stderr, "%s: Error opening the output file: %s\n",
					argv[optind], strerror(errno));
		goto err;
	}

	// Try to decompress all files.
	ret = decompress(&strm, infile, outfile, &lzma_err, &filein_err, &fileout_err);

	fprintf(stderr, "\n");

	if(ret != 0) {
		if(ret == 1) {
			fprintf(stderr, "%s: Error decompress the file: %s\n",
						argv[0], lzma_strerror(lzma_err));
		} else if(ret == 2) {
			fprintf(stderr, "%s: Error read the input file: %s\n",
						argv[0], strerror(filein_err));			
		} else {
			fprintf(stderr, "%s: Error write the output file: %s\n",
						argv[0], strerror(fileout_err));	
		}
		goto err;
	}

	// Free the memory allocated for the decoder. This only needs to be
	// done after the last file.
	lzma_end(&strm);

	if (fclose(infile)) {
		fprintf(stderr, "%s: Read error: %s\n", argv[0], strerror(errno));
		goto err;
	}
	infile = NULL;

	if (fclose(outfile)) {
		fprintf(stderr, "%s: Write error: %s\n", argv[optind], strerror(errno));
		goto err;
	}
	outfile = NULL;

	return EXIT_SUCCESS;

err:
	if(NULL != infile) {
		fclose(infile);
	}
	if(NULL != outfile) {
		fclose(outfile);
	}
	lzma_end(&strm);

	return EXIT_FAILURE;
}


int32_t main(int32_t argc, char ** argv)
{
	load_mode();
	if(0 == operation_mode) {
		// compress
		return compress_main(argc, argv);
	} else {
		// decompress
		return decompress_main(argc, argv);
	}
}