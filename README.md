# VMEM test driver

## Dependencis

* oneAPI

```
source /opt/intel/oneapi/setvars.sh
```

## Build All
```
make
```
### Build KMD
```
make driver
```

### Build UMD
```
make lib
```

### Build test
```
make test
```

## Run Test
```
insmod driver/vmem_drv.ko

# Need to disable compression for known issue
NEOReadDebugKeys=1 RenderCompressedBuffersEnabled=0 mpirun -n 2 test/test_vmem
```
