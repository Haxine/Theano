#section init_code_struct
prev_algo.algo = PARAMS->conv_algo;
prev_algo.mathType = CUDNN_DEFAULT_MATH;
reuse_algo = 0;
hash_prefix = std::string("GW|GPU#");

#section support_code_struct
#line 9 "dnn_gw.c"
int     reuse_algo;
AlgoRec prev_algo;
std::string hash_prefix;

#define THEANO_DONT_MEMSET_STRUCT

#ifdef DEBUG
char algorithm_name[128];
#endif

/** Check given algorithm against inputs and convolution descriptor,
    change algorithm inplace to a fallback algorithm if checkings fail.
    Return 0 on success, non-0 on error. **/
int dnn_conv_gw_fallback(cudnnConvolutionBwdFilterAlgo_t* _algo,
                         const PyGpuArrayObject* input,
                         const PyGpuArrayObject* kerns,
                         cudnnConvolutionDescriptor_t desc) {
  cudnnConvolutionBwdFilterAlgo_t algo = *_algo;

  // The FFT implementation does not support strides, 1x1 filters or inputs
  // with a spatial dimension larger than 1024.
  // If the chosen implementation is FFT, validate that it can
  // be used on the current data and default to a safe implementation if it
  // can't.
  // The following code is 2d-specific but it is fine as FFT and tiled-FFT are
  // defined only for 2d filters
  if (algo == CUDNN_CONVOLUTION_BWD_FILTER_ALGO_FFT &&
      PyGpuArray_NDIM(input) == 4) {
    // Extract the properties of the convolution descriptor
    int nd;
    int pad[2];
    int stride[2];
    int upscale[2];
    cudnnConvolutionMode_t mode;
    cudnnDataType_t data_type;
    cudnnStatus_t err = cudnnGetConvolutionNdDescriptor(desc, 2, &nd, pad, stride, upscale, &mode, &data_type);
    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError, "error getting convolution properties: %s",
                   cudnnGetErrorString(err));
      return 1;
    }

    if (stride[0] != 1 || stride[1] != 1 ||
        PyGpuArray_DIM(input, 2) > 1024 || PyGpuArray_DIM(input, 3) > 1024 ||
        (PyGpuArray_DIM(kerns, 2) == 1 && PyGpuArray_DIM(kerns, 3) == 1)) {
      algo = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0;
      #ifdef DEBUG
      fprintf(stderr, "(replacing gradweight algo fft with none)\n");
      #endif
    }
  }
  *_algo = algo;
  return 0;
}

int
APPLY_SPECIFIC(conv_gw)(PyGpuArrayObject *input, PyGpuArrayObject *output,
                        PyGpuArrayObject *km,
                        cudnnConvolutionDescriptor_t desc,
                        double alpha, double beta, PyGpuArrayObject **kerns,
                        PARAMS_TYPE* params) {
  PyGpuContextObject *c = input->context;
  void *alpha_p;
  void *beta_p;
  float af = alpha, bf = beta;
  cudnnStatus_t err = CUDNN_STATUS_SUCCESS;
  bool use_cached = 0;

  if (PyGpuArray_DIMS(input)[1] != PyGpuArray_DIMS(km)[1] * params->num_groups) {
    PyErr_SetString(PyExc_ValueError,
                    "GpuDnnConv images and kernel must have the same stack size");
    return 1;
  }
  if ((PyGpuArray_DIMS(output)[1] % params->num_groups) != 0) {
    PyErr_SetString(PyExc_ValueError,
		    "Number of output channels must be divisible by number of groups");
    return 1;
  }

  switch (input->ga.typecode) {
  case GA_DOUBLE:
    alpha_p = (void *)&alpha;
    beta_p = (void *)&beta;
    break;
  case GA_FLOAT:
  case GA_HALF:
    alpha_p = (void *)&af;
    beta_p = (void *)&bf;
    break;
  default:
    PyErr_SetString(PyExc_TypeError, "Unsupported type in convolution");
    return 1;
  }

  if (params->inplace) {
    Py_XDECREF(*kerns);
    *kerns = km;
    Py_INCREF(*kerns);
  } else {
    if (theano_prep_output(kerns, PyGpuArray_NDIM(km), PyGpuArray_DIMS(km),
                           km->ga.typecode, GA_C_ORDER, c) != 0)
      return 1;
    if (beta != 0.0 && pygpu_move(*kerns, km))
      return 1;
  }

  if (PyGpuArray_DIMS(input)[0] == 0 || PyGpuArray_DIMS(km)[0] == 0 || PyGpuArray_DIMS(km)[1] == 0) {
    int err2 = GpuArray_memset(&(*kerns)->ga, 0);
    if (err2 != GA_NO_ERROR) {
        PyErr_Format(PyExc_RuntimeError,
                     "GpuDnnConv grad wrt. weights could not fill the output with zeros: %d", err2);
        return 1;
    }
    return 0;
  }

  int groups = c_get_groups_for_conv(desc, params->num_groups);
  if (groups == -1)
    return 1;
  if (c_set_tensor_for_conv(input, APPLY_SPECIFIC(input), groups) == -1)
    return 1;
  if (c_set_tensor_for_conv(output, APPLY_SPECIFIC(output), groups) == -1)
    return 1;
  if (c_set_filter(*kerns, APPLY_SPECIFIC(kerns), groups) == -1)
    return 1;

  if (0 != dnn_check_convolution_output(desc, APPLY_SPECIFIC(input), APPLY_SPECIFIC(kerns),
                                        PyGpuArray_NDIM(*kerns), output, groups))
    return 1;

  size_t input_offset = PyGpuArray_STRIDE(input, 0) / groups;
  size_t kern_offset = PyGpuArray_STRIDE(*kerns, 0) * PyGpuArray_DIM(*kerns, 0) / groups;
  size_t output_offset = PyGpuArray_STRIDE(output, 0) / groups;

  cudnnConvolutionBwdFilterAlgo_t algo = params->conv_algo;
  size_t worksize = 0;
  cudnnMathType_t mathtype = CUDNN_DEFAULT_MATH;

  std::string hashkey ;

  size_t maxfree = c_get_largest_free_block_size(c);
  if (PyErr_Occurred()) return 1;

  cuda_enter(c->ctx);

  if (params->choose_algo) {

    if (!reuse_algo) {
      char pci_id[16];
      gpucontext_property(c->ctx, GA_CTX_PROP_PCIBUSID, pci_id);
      // check out cache
      hashkey = dnn_conv_shape(APPLY_SPECIFIC(input), input, APPLY_SPECIFIC(kerns), *kerns, desc, output, groups);
      if (hashkey.empty()) {
        cuda_exit(c->ctx);
        return 1;
      }
      hashkey = hash_prefix + pci_id + (params->choose_time ? " -t " : " ") + hashkey;
      const AlgoRec* cached = dnn_conv_check_cache(hashkey);
      if (cached) {
        prev_algo = *cached;
        use_cached = 1;
      }
    }

    if (reuse_algo || use_cached) {
      algo = (cudnnConvolutionBwdFilterAlgo_t)prev_algo.algo;
      worksize = prev_algo.wsSize;
      mathtype = prev_algo.mathType;
    } else {
      if (params->choose_time) {
        int count;
        cudnnConvolutionBwdFilterAlgoPerf_t choice;
        gpudata *tmpmem;

        // set the 'tensor math ok' flag
        c_set_math_type_for_conv(desc, CUDNN_TENSOR_OP_MATH);

        tmpmem = gpudata_alloc(c->ctx, maxfree, NULL, 0, NULL);
        if (tmpmem == NULL) {
          PyErr_SetString(PyExc_MemoryError, "Could not allocate working GPU memory");
          cuda_exit(c->ctx);
          return -1;
        }

        err = cudnnFindConvolutionBackwardFilterAlgorithmEx(
          params->handle, APPLY_SPECIFIC(input), PyGpuArray_DEV_DATA(input),
          APPLY_SPECIFIC(output), PyGpuArray_DEV_DATA(output), desc,
          APPLY_SPECIFIC(kerns), PyGpuArray_DEV_DATA(*kerns),
          1, &count, &choice, *(void **)tmpmem, maxfree);
        gpudata_release(tmpmem);

        if (err != CUDNN_STATUS_SUCCESS) {
          PyErr_Format(PyExc_RuntimeError,
                       "error selecting convolution algo: %s",
                       cudnnGetErrorString(err));
          cuda_exit(c->ctx);
          return 1;
        }

        #ifdef DEBUG
        if (count == 0) {
            PyErr_SetString(PyExc_RuntimeError, "No best-timed conv gradweight algorithm found");
            cuda_exit(c->ctx);
            return 1;
        } else if (choice.status != CUDNN_STATUS_SUCCESS) {
            PyErr_Format(PyExc_RuntimeError,
                         "error getting best-timed gradweight algo: %s",
                         cudnnGetErrorString(choice.status));
            cuda_exit(c->ctx);
            return 1;
        } // Else, count is necessarly 1 for current implementation.
        #endif

        algo = choice.algo;
        prev_algo.algo = (int)algo;
        prev_algo.wsSize = worksize = choice.memory;
#if CUDNN_MAJOR >= 7
        prev_algo.mathType = mathtype = choice.mathType;
#endif

      } else {
        err = cudnnGetConvolutionBackwardFilterAlgorithm(
          params->handle, APPLY_SPECIFIC(input), APPLY_SPECIFIC(output),
          desc, APPLY_SPECIFIC(kerns),
          CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT, maxfree, &algo);
        if (err != CUDNN_STATUS_SUCCESS) {
          PyErr_Format(PyExc_RuntimeError,
                       "error selecting convolution algo: %s",
                       cudnnGetErrorString(err));
          cuda_exit(c->ctx);
          return 1;
        }
	    prev_algo.algo = algo;
	    // no tensor_op returned from Get()
	    prev_algo.mathType = mathtype = CUDNN_DEFAULT_MATH;
      }
    }
  } /* choose_algo */

  if (c_set_math_type_for_conv(desc, mathtype) == -1 ||
      dnn_conv_gw_fallback(&algo, input, *kerns, desc) != 0) {
    cuda_exit(c->ctx);
    return 1;
  }

  // if FindEx was used (choose_time), workspace size is set.
  if (!(reuse_algo || use_cached || params->choose_time))
  {
    err = cudnnGetConvolutionBackwardFilterWorkspaceSize(
      params->handle, APPLY_SPECIFIC(input), APPLY_SPECIFIC(output), desc,
      APPLY_SPECIFIC(kerns), algo, &worksize);
    if (err == CUDNN_STATUS_NOT_SUPPORTED) {
      // Fallback to none algo if not supported
#ifdef DEBUG
      if (0 != theano_enum_to_string_cudnnConvolutionBwdFilterAlgo_t(algo, algorithm_name)) {
        cuda_exit(c->ctx);
        return 1;
      }
      fprintf(stderr, "(error getting worksize for %s: falling back to CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0)\n",
              algorithm_name);
#endif
      algo = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0;
      err = cudnnGetConvolutionBackwardFilterWorkspaceSize(
        params->handle, APPLY_SPECIFIC(input), APPLY_SPECIFIC(output), desc,
        APPLY_SPECIFIC(kerns), algo, &worksize);
    }

    if (err != CUDNN_STATUS_SUCCESS) {
      PyErr_Format(PyExc_RuntimeError, "error getting worksize: %s",
                   cudnnGetErrorString(err));
      cuda_exit(c->ctx);
      return 1;
    }
  }

  if (params->choose_algo && (!params->choose_once || !reuse_algo)) {
    // algo may have changed due to fallback, we must update it.
    prev_algo.algo = algo;
    // save worksize for next time/cache
    prev_algo.wsSize = worksize;

    // Add to the cache
    dnn_conv_update_cache(hashkey, prev_algo);
  }

#ifdef DEBUG
  if (params->choose_algo) {
    if (0 != theano_enum_to_string_cudnnConvolutionBwdFilterAlgo_t(algo, algorithm_name)) {
      cuda_exit(c->ctx);
      return 1;
    }
    // NB: This is printed only when algorithm is chosen at runtime.
    fprintf(stderr, "(using %s %s%s%s%s, ws:%ld, hash:%s)\n",
            algorithm_name,
            params->choose_time ? "(timed)": "" ,
            reuse_algo ? "(reused)" : "",
            use_cached ? "(cache)": "",
            mathtype == CUDNN_TENSOR_OP_MATH ? "(tensor op)" : "",
            worksize,
            hashkey.c_str()
     );
  }
#endif

  if (params->choose_once) {
    reuse_algo = 1;
  }

  gpudata *workspace = 0;

  if (worksize != 0) {
    workspace = gpudata_alloc(c->ctx, worksize, NULL, 0, NULL);
    if (workspace == NULL) {
      PyErr_SetString(PyExc_RuntimeError, "Could not allocate working memory");
      cuda_exit(c->ctx);
      return 1;
    }
  }

  cuda_wait(input->ga.data, GPUARRAY_CUDA_WAIT_READ);
  cuda_wait(output->ga.data, GPUARRAY_CUDA_WAIT_READ);
  cuda_wait((*kerns)->ga.data, GPUARRAY_CUDA_WAIT_WRITE);

  for ( int g = 0; g < groups; g++) {
    err = cudnnConvolutionBackwardFilter(
      params->handle,
      alpha_p,
      APPLY_SPECIFIC(input), ((char *)PyGpuArray_DEV_DATA(input)) + input_offset * g ,
      APPLY_SPECIFIC(output), ((char *)PyGpuArray_DEV_DATA(output)) + output_offset * g,
      desc, algo, worksize == 0 ? NULL : *(void **)workspace, worksize,
      beta_p,
      APPLY_SPECIFIC(kerns), ((char *)PyGpuArray_DEV_DATA(*kerns)) + kern_offset * g);
  }

  if (worksize != 0)
    gpudata_release(workspace);

  cuda_record(input->ga.data, GPUARRAY_CUDA_WAIT_READ);
  cuda_record(output->ga.data, GPUARRAY_CUDA_WAIT_READ);
  cuda_record((*kerns)->ga.data, GPUARRAY_CUDA_WAIT_WRITE);

  cuda_exit(c->ctx);

  if (err != CUDNN_STATUS_SUCCESS) {
    PyErr_Format(PyExc_RuntimeError, "error doing cuDNN conv gradweight operation: %s",
                 cudnnGetErrorString(err));
    return 1;
  }
  return 0;
}
