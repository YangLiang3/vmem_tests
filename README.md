# VMEM test driver

## Dependencis

* oneAPI

```
source /opt/intel/oneapi/setvars.sh
```

## Build

### Build KMD
```
make -C driver/
```

### Build UMD
```
make -C lib/
```

### Build test
```
make -C test/
```

## Run Test
```
insmod driver/vmem_drv.ko

# Need to disable compression for known issue
NEOReadDebugKeys=1 RenderCompressedBuffersEnabled=0 mpirun -n 2 test/test_vmem
```
