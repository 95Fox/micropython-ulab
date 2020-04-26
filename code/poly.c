
/*
 * This file is part of the micropython-ulab project, 
 *
 * https://github.com/v923z/micropython-ulab
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jeff Epler for Adafruit Industries
 *               2019-2020 Zoltán Vörös
 *
*/

#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "ndarray.h"
#include "linalg.h"
#include "poly.h"

#if ULAB_POLY_MODULE
static bool object_is_nditerable(mp_obj_t o_in) {
    if(MP_OBJ_IS_TYPE(o_in, &ulab_ndarray_type) || 
      MP_OBJ_IS_TYPE(o_in, &mp_type_tuple) || 
      MP_OBJ_IS_TYPE(o_in, &mp_type_list) || 
      MP_OBJ_IS_TYPE(o_in, &mp_type_range)) {
        return true;
    }
    return false;
}

static mp_obj_t poly_polyval(mp_obj_t o_p, mp_obj_t o_x) {
    // TODO: return immediately, if o_p is not an iterable
    // TODO: there is a bug here: matrices won't work, 
    // because there is a single iteration loop
    size_t m, n;
    if(MP_OBJ_IS_TYPE(o_x, &ulab_ndarray_type)) {
        ndarray_obj_t *ndx = MP_OBJ_TO_PTR(o_x);
        m = ndx->m;
        n = ndx->n;
    } else {
        mp_obj_array_t *ix = MP_OBJ_TO_PTR(o_x);
        m = 1;
        n = ix->len;
    }
    // polynomials are going to be of type float, except, when both 
    // the coefficients and the independent variable are integers
    ndarray_obj_t *out = create_new_ndarray(m, n, NDARRAY_FLOAT);
    mp_obj_iter_buf_t x_buf;
    mp_obj_t x_item, x_iterable = mp_getiter(o_x, &x_buf);

    mp_obj_iter_buf_t p_buf;
    mp_obj_t p_item, p_iterable;

    mp_float_t x, y;
    mp_float_t *outf = (mp_float_t *)out->array->items;
    uint8_t plen = mp_obj_get_int(mp_obj_len_maybe(o_p));
    mp_float_t *p = m_new(mp_float_t, plen);
    p_iterable = mp_getiter(o_p, &p_buf);
    uint16_t i = 0;    
    while((p_item = mp_iternext(p_iterable)) != MP_OBJ_STOP_ITERATION) {
        p[i] = mp_obj_get_float(p_item);
        i++;
    }
    i = 0;
    while ((x_item = mp_iternext(x_iterable)) != MP_OBJ_STOP_ITERATION) {
        x = mp_obj_get_float(x_item);
        y = p[0];
        for(uint8_t j=0; j < plen-1; j++) {
            y *= x;
            y += p[j+1];
        }
        outf[i++] = y;
    }
    m_del(mp_float_t, p, plen);
    return MP_OBJ_FROM_PTR(out);
}

MP_DEFINE_CONST_FUN_OBJ_2(poly_polyval_obj, poly_polyval);

static mp_obj_t poly_polyfit(size_t  n_args, const mp_obj_t *args) {
    if((n_args != 2) && (n_args != 3)) {
        mp_raise_ValueError(translate("number of arguments must be 2, or 3"));
    }
    if(!object_is_nditerable(args[0])) {
        mp_raise_ValueError(translate("input data must be an iterable"));
    }
    uint16_t lenx = 0, leny = 0;
    uint8_t deg = 0;
    mp_float_t *x, *XT, *y, *prod;

    if(n_args == 2) { // only the y values are supplied
        // TODO: this is actually not enough: the first argument can very well be a matrix, 
        // in which case we are between the rock and a hard place
        leny = (uint16_t)mp_obj_get_int(mp_obj_len_maybe(args[0]));
        deg = (uint8_t)mp_obj_get_int(args[1]);
        if(leny < deg) {
            mp_raise_ValueError(translate("more degrees of freedom than data points"));
        }
        lenx = leny;
        x = m_new(mp_float_t, lenx); // assume uniformly spaced data points
        for(size_t i=0; i < lenx; i++) {
            x[i] = i;
        }
        y = m_new(mp_float_t, leny);
        fill_array_iterable(y, args[0]);
    } else /* n_args == 3 */ {
        if(!object_is_nditerable(args[1])) {
            mp_raise_ValueError(translate("input data must be an iterable"));
        }
        lenx = (uint16_t)mp_obj_get_int(mp_obj_len_maybe(args[0]));
        leny = (uint16_t)mp_obj_get_int(mp_obj_len_maybe(args[1]));
        if(lenx != leny) {
            mp_raise_ValueError(translate("input vectors must be of equal length"));
        }
        deg = (uint8_t)mp_obj_get_int(args[2]);
        if(leny < deg) {
            mp_raise_ValueError(translate("more degrees of freedom than data points"));
        }
        x = m_new(mp_float_t, lenx);
        fill_array_iterable(x, args[0]);
        y = m_new(mp_float_t, leny);
        fill_array_iterable(y, args[1]);
    }
    
    // one could probably express X as a function of XT, 
    // and thereby save RAM, because X is used only in the product
    XT = m_new(mp_float_t, (deg+1)*leny); // XT is a matrix of shape (deg+1, len) (rows, columns)
    for(uint8_t i=0; i < leny; i++) { // column index
        XT[i+0*lenx] = 1.0; // top row
        for(uint8_t j=1; j < deg+1; j++) { // row index
            XT[i+j*leny] = XT[i+(j-1)*leny]*x[i];
        }
    }
    
    prod = m_new(mp_float_t, (deg+1)*(deg+1)); // the product matrix is of shape (deg+1, deg+1)
    mp_float_t sum;
    for(uint16_t i=0; i < deg+1; i++) { // column index
        for(uint16_t j=0; j < deg+1; j++) { // row index
            sum = 0.0;
            for(size_t k=0; k < lenx; k++) {
                // (j, k) * (k, i) 
                // Note that the second matrix is simply the transpose of the first: 
                // X(k, i) = XT(i, k) = XT[k*lenx+i]
                sum += XT[j*lenx+k]*XT[i*lenx+k]; // X[k*(deg+1)+i];
            }
            prod[j*(deg+1)+i] = sum;
        }
    }
    if(!linalg_invert_matrix(prod, deg+1)) {
        // Although X was a Vandermonde matrix, whose inverse is guaranteed to exist, 
        // we bail out here, if prod couldn't be inverted: if the values in x are not all 
        // distinct, prod is singular
        m_del(mp_float_t, XT, (deg+1)*lenx);
        m_del(mp_float_t, x, lenx);
        m_del(mp_float_t, y, lenx);
        m_del(mp_float_t, prod, (deg+1)*(deg+1));
        mp_raise_ValueError(translate("could not invert Vandermonde matrix"));
    } 
    // at this point, we have the inverse of X^T * X
    // y is a column vector; x is free now, we can use it for storing intermediate values
    for(uint16_t i=0; i < deg+1; i++) { // row index
        sum = 0.0;
        for(uint16_t j=0; j < lenx; j++) { // column index
            sum += XT[i*lenx+j]*y[j];
        }
        x[i] = sum;
    }
    // XT is no longer needed
    m_del(mp_float_t, XT, (deg+1)*leny);
    
    ndarray_obj_t *beta = create_new_ndarray(deg+1, 1, NDARRAY_FLOAT);
    mp_float_t *betav = (mp_float_t *)beta->array->items;
    // x[0..(deg+1)] contains now the product X^T * y; we can get rid of y
    m_del(float, y, leny);
    
    // now, we calculate beta, i.e., we apply prod = (X^T * X)^(-1) on x = X^T * y; x is a column vector now
    for(uint16_t i=0; i < deg+1; i++) {
        sum = 0.0;
        for(uint16_t j=0; j < deg+1; j++) {
            sum += prod[i*(deg+1)+j]*x[j];
        }
        betav[i] = sum;
    }
    m_del(mp_float_t, x, lenx);
    m_del(mp_float_t, prod, (deg+1)*(deg+1));
    for(uint8_t i=0; i < (deg+1)/2; i++) {
        // We have to reverse the array, for the leading coefficient comes first. 
        SWAP(mp_float_t, betav[i], betav[deg-i]);
    }
    return MP_OBJ_FROM_PTR(beta);
}

MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(poly_polyfit_obj, 2, 3, poly_polyfit);

mp_obj_t poly_interp(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = mp_const_none } },
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = mp_const_none } },
        { MP_QSTR_, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = mp_const_none } },
        { MP_QSTR_left, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_right, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
    };
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
	
	ndarray_obj_t *x = ndarray_from_mp_obj(args[0].u_obj);
	ndarray_obj_t *xp = ndarray_from_mp_obj(args[1].u_obj); // xp must hold an increasing sequence of independent values
	ndarray_obj_t *fp = ndarray_from_mp_obj(args[2].u_obj);
	// TODO: check if the shape is (1, n), or (m, 1)
	if(((xp->m != 1) && (xp->n != 1)) || ((fp->m != 1) && (fp->n != 1)) || 
		(xp->array->len < 2) || (fp->array->len < 2) || (xp->array->len != fp->array->len)) {
		mp_raise_ValueError(translate("interp is defined for 1D arrays of equal length"));
	}
	ndarray_obj_t *y = create_new_ndarray(x->m, x->n, NDARRAY_FLOAT);
	mp_float_t left_value, right_value;
	mp_float_t xp_left = ndarray_get_float_value(xp->array->items, xp->array->typecode, 0);
	mp_float_t xp_right = ndarray_get_float_value(xp->array->items, xp->array->typecode, xp->array->len-1);
	if(args[3].u_obj == mp_const_none) {
		left_value = ndarray_get_float_value(fp->array->items, fp->array->typecode, 0);
	} else {
		left_value = mp_obj_get_float(args[3].u_obj);
	}
	if(args[4].u_obj == mp_const_none) {
		right_value = ndarray_get_float_value(fp->array->items, fp->array->typecode, fp->array->len-1);
	} else {
		right_value = mp_obj_get_float(args[4].u_obj);
	}
	mp_float_t *yarray = (mp_float_t *)y->array->items;
	for(size_t i=0; i < x->array->len; i++, yarray++) {
		mp_float_t x_value = ndarray_get_float_value(x->array->items, x->array->typecode, i);
		if(x_value <= xp_left) {
			*yarray = left_value;
		} else if(x_value >= xp_right) {
			*yarray = right_value;
		} else { // do the binary search here
			mp_float_t xp_left_, xp_right_;
			mp_float_t fp_left, fp_right;
			size_t left_index = 0, right_index = xp->array->len - 1, middle_index;
			while(right_index - left_index > 1) {
				middle_index = left_index + (right_index - left_index) / 2;
				mp_float_t xp_middle = ndarray_get_float_value(xp->array->items, xp->array->typecode, middle_index);
				if(x_value <= xp_middle) {
					right_index = middle_index;
				} else {
					left_index = middle_index;
				}
			}
			xp_left_ = ndarray_get_float_value(xp->array->items, xp->array->typecode, left_index);
			xp_right_ = ndarray_get_float_value(xp->array->items, xp->array->typecode, right_index);
			fp_left = ndarray_get_float_value(fp->array->items, fp->array->typecode, left_index);
			fp_right = ndarray_get_float_value(fp->array->items, fp->array->typecode, right_index);
			*yarray = fp_left + (x_value - xp_left_) * (fp_right - fp_left) / (xp_right_ - xp_left_);
		}
	}
	return MP_OBJ_FROM_PTR(y);
}

MP_DEFINE_CONST_FUN_OBJ_KW(poly_interp_obj, 2, poly_interp);

STATIC const mp_rom_map_elem_t ulab_poly_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_poly) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_polyval), (mp_obj_t)&poly_polyval_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_polyfit), (mp_obj_t)&poly_polyfit_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_interp), (mp_obj_t)&poly_interp_obj },    
};

STATIC MP_DEFINE_CONST_DICT(mp_module_ulab_poly_globals, ulab_poly_globals_table);

mp_obj_module_t ulab_poly_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_ulab_poly_globals,
};

#endif
