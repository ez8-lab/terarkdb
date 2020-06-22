#!/usr/bin/python
#
#
# TerarkDB use this test as performance baseline
#
# Memory=64GB, Raw Data Size=512GB
#
import time
import io
import os
import subprocess
import sys

# how many config string do you want to test
BENCH_CONFIGS=['TerarkZipTable_max_open_files=-1;TerarkZipTable_target_file_size_base=64m;TerarkZipTable_target_file_size_multiplier=1;TerarkZipTable_disableCompressDict=true;TerarkZipTable_offsetArrayBlockUnits=128;TerarkZipTable_sampleRatio=0.005;TerarkZipTable_compaction_style=Level;TerarkZipTable_entropyAlgo=huf;DictZipBlobStore_zipThreads=0;TerarkZipTable_max_subcompactions=8;TerarkZipTable_max_background_flushes=4;TerarkZipTable_max_background_compactions=6;TerarkZipTable_max_background_garbage_collections=4;TerarkZipTable_level0_file_num_compaction_trigger=4;TerarkZipTable_level0_slowdown_writes_trigger=1000;TerarkZipTable_level0_stop_writes_trigger=1000;TerarkZipTable_max_compaction_bytes=256M;TerarkZipTable_max_write_buffer_number=16;TerarkZipTable_blob_size=-1;TerarkZipTable_warmUpIndexOnOpen=false;TerarkZipTable_minPreadLen=128;TerarkDB_FileReaderUseFsRead=1', '']
VALUE_SIZES = [512, 4096]


# dont need to change
TOTAL_DB_SIZE_IN_GB = 512
TOTAL_MEM_IN_GB = 64
THREADS = 16
DB_DIR = ""
TERARK_CONFIG_STRING = ''


def bench(records, value_size, bench_type, exist_db):
    cmd = """
       env TerarkConfigString='{config_string}' ./db_bench --benchmarks={bench_type}
	   --use_existing_db={exist_db}
           --sync=0
	   --db={db_dir}
	   --wal_dir={db_dir}
           --num={records}
           --threads={threads}
	   --num_levels=6
           --delayed_write_rate=134217728
	   --blob_size=128
	   --key_size=20
	   --value_size={value_size}
	   --cache_numshardbits=6
	   --level_compaction_dynamic_level_bytes=true
	   --bytes_per_sync=8388608
	   --cache_index_and_filter_blocks=0
	   --pin_l0_filter_and_index_blocks_in_cache=1
	   --benchmark_write_rate_limit=0
	   --hard_rate_limit=3
	   --rate_limit_delay_max_milliseconds=1000000
	   --write_buffer_size=134217728
	   --target_file_size_base=134217728
	   --max_bytes_for_level_base=1073741824
	   --verify_checksum=1
	   --delete_obsolete_files_period_micros=62914560
	   --max_bytes_for_level_multiplier=8
	   --statistics=0
	   --stats_per_interval=1
	   --stats_interval_seconds=60
	   --histogram=1
	   --open_files=-1
	   --level0_file_num_compaction_trigger=4
	   --level0_slowdown_writes_trigger=1000
	   --level0_stop_writes_trigger=1000
           """.format(records=records, 
                      value_size=value_size, 
                      db_dir=DB_DIR, 
                      bench_type=bench_type, 
                      exist_db=exist_db,
                      threads=THREADS,
                      config_string=TERARK_CONFIG_STRING)

    cmd = cmd.replace('\n',' ')
    filename = 'log_%s_%s' % (bench_type, value_size)
    log = open(filename, 'wb')
    log.write(cmd)
    log.flush()
    process = subprocess.Popen(cmd,
                               stdin=subprocess.PIPE,
                               stderr=log, 
                               stdout=log, 
                               shell=True)
    process.communicate()
    log.flush()
    log.close()


def run():
    db_size_bytes = int(TOTAL_DB_SIZE_IN_GB) * 1024 * 1024 * 1024
    for vsize in VALUE_SIZES:
        records = db_size_bytes / vsize
        bench(records, vsize, "fillrandom", 0)
        bench(records, vsize, "readrandomwriterandom", 1)


def gather_result():
    rst = {}
    for bench_type in ['fillrandom', 'readrandomwriterandom']:
        rst[bench_type] = {}
        
        for vsize in VALUE_SIZES:
            rst[bench_type][vsize] = {}

            filename = "log_%s_%s" % (bench_type, vsize)
            with open(filename, 'rb') as f:
                lines = f.readlines()
                i = 0
                while i < len(lines):
                    # get ops
                    s = '%s' % bench_type
                    if lines[i].find(s) == 0:
                        rst[bench_type][vsize]['ops'] = lines[i].split()[4]

                    # get rest of them
                    s = 'Microseconds per '
                    if lines[i].find(s) >= 0:
                        ops_type = lines[i][17:-2]
                        rst[bench_type][vsize][ops_type] = {}
                        rst[bench_type][vsize][ops_type]['max'] = lines[i+2].split()[5]
                        rst[bench_type][vsize][ops_type]['percentiles'] = lines[i+3][13:-2]
                        #print rst
                        i = i + 5
                    else:
                        i = i + 1

    # print rst
    output = [('benchmark', 'val size', 'ops', 'operation', 'max lat(us)', 'pct(us)')]
    for bench in rst:
        for vsize in rst[bench]:
            for t in ['read', 'write']:
                if rst[bench][vsize].has_key(t):
                    if not rst[bench][vsize].has_key('printed'):
                        output.append( (bench, '%s Bytes'%vsize, '%s /sec' % rst[bench][vsize]['ops'], t, rst[bench][vsize][t]['max'], rst[bench][vsize][t]['percentiles']) )
                        rst[bench][vsize]['printed'] = 1
                    else:
                        output.append( ('', '', '', t, rst[bench][vsize][t]['max'], rst[bench][vsize][t]['percentiles']) )

    with open('log.txt', 'a') as f:
        for row in output:
            f.write('{0:<25} {1:<15} {2:<15} {3:<15} {4:<15} {5:<100}\n'.format(*row))



if __name__=='__main__':
    if len(sys.argv) != 4:
        print 'usage: ./bench_baseline.py [DB_DIR] [DB_SIZE_GB] [THREADS]'
        sys.exit()

    DB_DIR = sys.argv[1]
    TOTAL_DB_SIZE_IN_GB = sys.argv[2]
    THREADS = sys.argv[3]

    with open('log.txt', 'a') as f:
        f.write('DB_SIZE(GB): %s, THREADS = %s \n' % (TOTAL_DB_SIZE_IN_GB, THREADS) )


    for config in BENCH_CONFIGS:
        with open('log.txt', 'a') as f:
            f.write('vvvvvvvvvv config vvvvvvvvvv\n')
            f.write(config)
            f.write('\n^^^^^^^^^^ config ^^^^^^^^^^\n')

        TERARK_CONFIG_STRING = config
        run()
        gather_result()


"""

#TerarkZipTable_target_file_size_multiplier=1;TerarkZipTable_disableCompressDict=true;TerarkZipTable_offsetArrayBlockUnits=128;TerarkZipTable_sampleRatio=0.005;TerarkZipTable_compaction_style=Level;TerarkZipTable_entropyAlgo=huf;DictZipBlobStore_zipThreads=0;TerarkZipTable_max_subcompactions=8;TerarkZipTable_max_background_flushes=4;TerarkZipTable_max_background_compactions=10;TerarkZipTable_max_background_garbage_collections=6;TerarkZipTable_level0_file_num_compaction_trigger=4;TerarkZipTable_level0_slowdown_writes_trigger=1000;TerarkZipTable_level0_stop_writes_trigger=1000;TerarkZipTable_max_compaction_bytes=256M;TerarkZipTable_max_write_buffer_number=16;TerarkZipTable_blob_size=-1;TerarkZipTable_warmUpIndexOnOpen=false'

"""
