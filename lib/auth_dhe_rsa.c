/*
 *      Copyright (C) 2000,2001 Nikos Mavroyanopoulos
 *
 * This file is part of GNUTLS.
 *
 * GNUTLS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUTLS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */


#include "gnutls_int.h"
#include "gnutls_auth_int.h"
#include "gnutls_errors.h"
#include "gnutls_dh.h"
#include "gnutls_num.h"
#include "gnutls_sig.h"
#include <gnutls_datum.h>
#include <auth_x509.h>

static int gen_dhe_rsa_server_kx(GNUTLS_STATE, opaque **);
static int gen_dhe_rsa_client_kx(GNUTLS_STATE, opaque **);
static int proc_dhe_rsa_server_kx(GNUTLS_STATE, opaque *, int);
static int proc_dhe_rsa_client_kx(GNUTLS_STATE, opaque *, int);

MOD_AUTH_STRUCT dhe_rsa_auth_struct = {
	"DHE_RSA",
	_gnutls_gen_x509_server_certificate,
	_gnutls_gen_x509_client_certificate,
	gen_dhe_rsa_server_kx,
	NULL,
	NULL,
	gen_dhe_rsa_client_kx,
	_gnutls_gen_x509_client_cert_vrfy,	/* gen client cert vrfy */
	_gnutls_gen_x509_server_cert_req,	/* server cert request */

	_gnutls_proc_x509_server_certificate,
	_gnutls_proc_x509_client_certificate,
	proc_dhe_rsa_server_kx,
	NULL,
	NULL,
	proc_dhe_rsa_client_kx,
	_gnutls_proc_x509_client_cert_vrfy,	/* proc client cert vrfy */
	_gnutls_proc_x509_cert_req	/* proc server cert request */
};

static int gen_dhe_rsa_server_kx(GNUTLS_STATE state, opaque ** data)
{
	MPI x, X, g, p;
	size_t n_X, n_g, n_p;
	uint8 *data_p;
	uint8 *data_g;
	uint8 *data_X;
	int ret = 0, data_size;
	const X509PKI_CREDENTIALS cred;
	int bits;
	gnutls_cert *apr_cert_list;
	gnutls_private_key *apr_pkey;
	int apr_cert_list_length;
	gnutls_datum signature, ddata;
	X509PKI_SERVER_AUTH_INFO info;

	cred =
	    _gnutls_get_kx_cred(state->gnutls_key, GNUTLS_X509PKI, NULL);
	if (cred == NULL) {
		bits = DEFAULT_BITS;	/* default */
	} else {
		bits = cred->dh_bits;
	}

	/* find the appropriate certificate */
	if ((ret=_gnutls_find_apr_cert( state, &apr_cert_list, &apr_cert_list_length, &apr_pkey))<0) {
		gnutls_assert();
		return ret;
	}

	g = gnutls_get_dh_params(&p, bits);
	if (g == NULL) {
		gnutls_assert();
		return GNUTLS_E_MEMORY_ERROR;
	}

	if ( state->gnutls_key->auth_info==NULL) {
		state->gnutls_key->auth_info =
		    gnutls_calloc(1, sizeof(X509PKI_SERVER_AUTH_INFO_INT));
		if (state->gnutls_key->auth_info == NULL)
			return GNUTLS_E_MEMORY_ERROR;

		state->gnutls_key->auth_info_size =
		    sizeof(X509PKI_SERVER_AUTH_INFO_INT);

		info = state->gnutls_key->auth_info;
		info->dh_bits = gcry_mpi_get_nbits(p);
		info->peer_certificate_status = GNUTLS_CERT_NONE;
	}
	info = state->gnutls_key->auth_info;
	
	X = gnutls_calc_dh_secret(&x, g, p);
	if (X == NULL) {
		_gnutls_mpi_release(&g);
		_gnutls_mpi_release(&p);
		gnutls_assert();
		return GNUTLS_E_MEMORY_ERROR;
	}
	state->gnutls_key->dh_secret = x;

	gcry_mpi_print(GCRYMPI_FMT_USG, NULL, &n_g, g);
	gcry_mpi_print(GCRYMPI_FMT_USG, NULL, &n_p, p);
	gcry_mpi_print(GCRYMPI_FMT_USG, NULL, &n_X, X);
	(*data) = gnutls_malloc(n_g + n_p + n_X + 6);
	if (*data == NULL) {
		_gnutls_mpi_release(&X);
		_gnutls_mpi_release(&g);
		_gnutls_mpi_release(&p);
		return GNUTLS_E_MEMORY_ERROR;
	}

	data_p = &(*data)[0];
	gcry_mpi_print(GCRYMPI_FMT_USG, &data_p[2], &n_p, p);
	_gnutls_mpi_release(&p);

	WRITEuint16(n_p, data_p);

	data_g = &data_p[2 + n_p];
	gcry_mpi_print(GCRYMPI_FMT_USG, &data_g[2], &n_g, g);
	_gnutls_mpi_release(&g);

	WRITEuint16(n_g, data_g);

	data_X = &data_g[2 + n_g];
	gcry_mpi_print(GCRYMPI_FMT_USG, &data_X[2], &n_X, X);
	_gnutls_mpi_release(&X);

	WRITEuint16(n_X, data_X);

	data_size = n_p + n_g + n_X + 6;


	/* Generate the signature. */

	/* If our certificate supports signing
	 */
	if (apr_cert_list != NULL)
		if (apr_cert_list[0].keyUsage != 0)
			if (!
			    (apr_cert_list[0].
			     keyUsage & X509KEY_DIGITAL_SIGNATURE)) {
				gnutls_assert();
				gnutls_free( *data);
				return GNUTLS_E_X509_KEY_USAGE_VIOLATION;
			}


	ddata.data = *data;
	ddata.size = data_size;

	if (apr_pkey != NULL) {
		if ((ret =
		     _gnutls_generate_sig_params(
		     	state, apr_pkey, &ddata, &signature)) < 0) {
			gnutls_assert();
			gnutls_free( *data);
			return ret;
		}
	} else {
		gnutls_assert();
		return data_size; /* do not put a signature - ILLEGAL! */
	}

	*data = gnutls_realloc( *data, data_size+signature.size+2);
	if (*data==NULL) {
		gnutls_assert();
		return GNUTLS_E_MEMORY_ERROR;
	}
	
	WRITEdatum16( &(*data)[data_size], signature);
	data_size += signature.size+2;

	return data_size;
}

static int gen_dhe_rsa_client_kx(GNUTLS_STATE state, opaque ** data)
{
	MPI x, X;
	size_t n_X;
	int ret;

	X = gnutls_calc_dh_secret(&x, state->gnutls_key->client_g,
				  state->gnutls_key->client_p);
	if (X==NULL)
		return GNUTLS_E_MEMORY_ERROR;

	gcry_mpi_print(GCRYMPI_FMT_USG, NULL, &n_X, X);
	(*data) = gnutls_malloc(n_X + 2);
	if (*data == NULL)
		return GNUTLS_E_MEMORY_ERROR;

	gcry_mpi_print(GCRYMPI_FMT_USG, &(*data)[2], &n_X, X);
	(*data)[0] = 1;		/* extern - explicit since we do not have
				   certificate */
	_gnutls_mpi_release(&X);

	WRITEuint16(n_X, &(*data)[0]);

	/* calculate the key after calculating the message */
	state->gnutls_key->KEY =
	    gnutls_calc_dh_key(state->gnutls_key->client_Y, x,
			       state->gnutls_key->client_p);
	if (state->gnutls_key->KEY==NULL)
		return GNUTLS_E_MEMORY_ERROR;


	/* THESE SHOULD BE DISCARDED */
	_gnutls_mpi_release(&state->gnutls_key->client_Y);
	_gnutls_mpi_release(&state->gnutls_key->client_p);
	_gnutls_mpi_release(&state->gnutls_key->client_g);

	ret = _gnutls_generate_key( state->gnutls_key);
	_gnutls_mpi_release(&state->gnutls_key->KEY);

	if (ret < 0) {
		return ret;
	}

	return n_X + 2;
}

static int proc_dhe_rsa_server_kx(GNUTLS_STATE state, opaque * data,
				  int data_size)
{
	uint16 n_Y, n_g, n_p;
	size_t _n_Y, _n_g, _n_p;
	uint8 *data_p;
	uint8 *data_g;
	uint8 *data_Y;
	int i, sigsize;
	gnutls_datum vparams, signature;
	int ret;

	i = 0;
	n_p = READuint16(&data[i]);
	i += 2;

	data_p = &data[i];
	i += n_p;
	if (i > data_size) {
		gnutls_assert();
		return GNUTLS_E_UNEXPECTED_PACKET_LENGTH;
	}

	n_g = READuint16(&data[i]);
	i += 2;

	data_g = &data[i];
	i += n_g;
	if (i > data_size) {
		gnutls_assert();
		return GNUTLS_E_UNEXPECTED_PACKET_LENGTH;
	}

	n_Y = READuint16(&data[i]);
	i += 2;

	data_Y = &data[i];
	i += n_Y;
	if (i > data_size) {
		gnutls_assert();
		return GNUTLS_E_UNEXPECTED_PACKET_LENGTH;
	}
	_n_Y = n_Y;
	_n_g = n_g;
	_n_p = n_p;

	if (gcry_mpi_scan(&state->gnutls_key->client_Y,
			  GCRYMPI_FMT_USG, data_Y, &_n_Y) != 0) {
		gnutls_assert();
		return GNUTLS_E_MPI_SCAN_FAILED;
	}

	if (gcry_mpi_scan(&state->gnutls_key->client_g,
			  GCRYMPI_FMT_USG, data_g, &_n_g) != 0) {
		gnutls_assert();
		return GNUTLS_E_MPI_SCAN_FAILED;
	}
	if (gcry_mpi_scan(&state->gnutls_key->client_p,
			  GCRYMPI_FMT_USG, data_p, &_n_p) != 0) {
		gnutls_assert();
		return GNUTLS_E_MPI_SCAN_FAILED;
	}


	/* VERIFY SIGNATURE */
	
	vparams.size = n_Y + n_p + n_g + 6;
	vparams.data = data;

	if (data_size-vparams.size-2 <= 0) { /* check if the peer sent enough data */
		gnutls_assert();
		return GNUTLS_E_UNEXPECTED_PACKET_LENGTH;
	}
	
	sigsize = READuint16( &data[vparams.size]);
	signature.data = &data[vparams.size+2];
	signature.size = GMIN(data_size-vparams.size-2, sigsize);	

	if (state->gnutls_internals.peer_cert.version == 0) { /* this is the only way to check
							       * if it is initialized
							       */
		gnutls_assert();
		return GNUTLS_E_X509_CERTIFICATE_ERROR;
	}
	ret = _gnutls_verify_sig_params( state, &state->gnutls_internals.peer_cert, &vparams, &signature);
	if (ret<0) {
		gnutls_assert();
		return ret;
	}

	return ret;
}

static int proc_dhe_rsa_client_kx(GNUTLS_STATE state, opaque * data,
				  int data_size)
{
	uint16 n_Y;
	size_t _n_Y;
	MPI g, p;
	const X509PKI_CREDENTIALS cred;
	int bits, ret;

	cred =
	    _gnutls_get_kx_cred(state->gnutls_key, GNUTLS_X509PKI, NULL);
	if (cred == NULL) {
		bits = DEFAULT_BITS;	/* default */
	} else {
		bits = cred->dh_bits;
	}


	n_Y = READuint16(&data[0]);
	_n_Y = n_Y;

	if (gcry_mpi_scan(&state->gnutls_key->client_Y,
			  GCRYMPI_FMT_USG, &data[2], &_n_Y)) {
		gnutls_assert();
		return GNUTLS_E_MPI_SCAN_FAILED;
	}

	g = gnutls_get_dh_params(&p, bits);
	if (g==NULL) {
		gnutls_assert();
		return GNUTLS_E_MEMORY_ERROR;
	}

	state->gnutls_key->KEY =
	    gnutls_calc_dh_key(state->gnutls_key->client_Y,
			       state->gnutls_key->dh_secret, p);
	if (state->gnutls_key->KEY==NULL)
		return GNUTLS_E_MEMORY_ERROR;


	_gnutls_mpi_release(&g);
	_gnutls_mpi_release(&p);

	_gnutls_mpi_release(&state->gnutls_key->client_Y);
	_gnutls_mpi_release(&state->gnutls_key->dh_secret);

	ret = _gnutls_generate_key( state->gnutls_key);
	_gnutls_mpi_release(&state->gnutls_key->KEY);

	if (ret < 0) {
		return ret;
	}

	return 0;
}
