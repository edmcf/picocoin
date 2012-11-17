
#include "picocoin-config.h"

#include <ccoin/script.h>
#include <ccoin/script_eval.h>
#include <ccoin/util.h>

static const size_t nMaxNumSize = 4;

bool bp_tx_sighash(bu256_t *hash, GString *scriptCode,
		   const struct bp_tx *txTo, unsigned int nIn,
		   int nHashType)
{
	if (!hash || !scriptCode || !txTo || !txTo->vin)
		return false;
	if (nIn >= txTo->vin->len)
		return false;
	
	bool rc = false;
	struct bp_tx txTmp;
	bp_tx_init(&txTmp);
	bp_tx_copy(&txTmp, txTo);

	/* TODO: find-and-delete OP_CODESEPARATOR from scriptCode */

	/* Blank out other inputs' signatures */
	unsigned int i;
	struct bp_txin *txin;
	for (i = 0; i < txTmp.vin->len; i++) {
		txin = g_ptr_array_index(txTmp.vin, i);
		g_string_set_size(txin->scriptSig, 0);

		if (i == nIn)
			g_string_append_len(txin->scriptSig,
					    scriptCode->str, scriptCode->len);
	}

	/* Blank out some of the outputs */
	if ((nHashType & 0x1f) == SIGHASH_NONE) {
		/* Wildcard payee */
		bp_tx_free_vout(&txTmp);
		txTmp.vout = g_ptr_array_new_full(1, g_free);

		/* Let the others update at will */
		for (i = 0; i < txTmp.vin->len; i++) {
			txin = g_ptr_array_index(txTmp.vin, i);
			if (i != nIn)
				txin->nSequence = 0;
		}
	}

	else if ((nHashType & 0x1f) == SIGHASH_SINGLE) {
		/* Only lock-in the txout payee at same index as txin */
		unsigned int nOut = nIn;
		if (nOut >= txTmp.vout->len)
			goto out;

		g_ptr_array_set_size(txTmp.vout, nOut + 1);

		for (i = 0; i < nOut; i++) {
			struct bp_txout *txout;

			txout = g_ptr_array_index(txTmp.vout, i);
			bp_txout_set_null(txout);
		}

		/* Let the others update at will */
		for (i = 0; i < txTmp.vin->len; i++) {
			txin = g_ptr_array_index(txTmp.vin, i);
			if (i != nIn)
				txin->nSequence = 0;
		}
	}

	/* Blank out other inputs completely;
	   not recommended for open transactions */
	if (nHashType & SIGHASH_ANYONECANPAY) {
		if (nIn > 0)
			g_ptr_array_remove_range(txTmp.vin, 0, nIn);
		g_ptr_array_set_size(txTmp.vin, 1);
	}

	/* Serialize and hash */
	bp_tx_calc_sha256(&txTmp);
	bu256_copy(hash, &txTmp.sha256);

	rc = true;

out:
	bp_tx_free(&txTmp);
	return rc;
}

static const unsigned char disabled_op[256] = {
	[OP_CAT] = 1,
	[OP_SUBSTR] = 1,
	[OP_LEFT] = 1,
	[OP_RIGHT] = 1,
	[OP_INVERT] = 1,
	[OP_AND] = 1,
	[OP_OR] = 1,
	[OP_XOR] = 1,
	[OP_2MUL] = 1,
	[OP_2DIV] = 1,
	[OP_MUL] = 1,
	[OP_DIV] = 1,
	[OP_MOD] = 1,
	[OP_LSHIFT] = 1,
	[OP_RSHIFT] = 1,
};

static bool CastToBigNum(BIGNUM *vo, const struct buffer *buf)
{
	if (buf->len > nMaxNumSize)
		return false;
	
	// Get rid of extra leading zeros:
	// buf -> bn -> buf -> bn

	BIGNUM bn;
	BN_init(&bn);

	bn_setvch(&bn, buf->p, buf->len);
	GString *bn_s = bn_getvch(&bn);

	bn_setvch(vo, bn_s->str, bn_s->len);

	g_string_free(bn_s, TRUE);
	BN_clear_free(&bn);
	return true;
}

static bool CastToBool(const struct buffer *buf)
{
	unsigned int i;
	const unsigned char *vch = buf->p;
	for (i = 0; i < buf->len; i++) {
		if (vch[i] != 0) {
			// Can be negative zero
			if (i == (buf->len - 1) && vch[i] == 0x80)
				return false;
			return true;
		}
	}

	return false;
}

static void stack_push(GPtrArray *stack, const struct buffer *buf)
{
	g_ptr_array_add(stack, buffer_copy(buf->p, buf->len));
}

static void stack_push_char(GPtrArray *stack, unsigned char ch)
{
	g_ptr_array_add(stack, buffer_copy(&ch, 1));
}

static void stack_push_str(GPtrArray *stack, GString *s)
{
	g_ptr_array_add(stack, buffer_copy(s->str, s->len));
	g_string_free(s, TRUE);
}

static struct buffer *stacktop(GPtrArray *stack, int index)
{
	return stack->pdata[stack->len + index];
}

static struct buffer *stack_take(GPtrArray *stack, int index)
{
	struct buffer *ret = stack->pdata[stack->len + index];
	stack->pdata[stack->len + index] = NULL;
	return ret;
}

static void popstack(GPtrArray *stack)
{
	if (stack->len)
		g_ptr_array_remove_index(stack, stack->len - 1);
}

static void stack_swap(GPtrArray *stack, int idx1, int idx2)
{
	int len = stack->len;
	struct buffer *tmp = stack->pdata[len + idx1];
	stack->pdata[len + idx1] = stack->pdata[len + idx2];
	stack->pdata[len + idx2] = tmp;
}

static unsigned int count_false(GByteArray *vfExec)
{
	unsigned int i, count = 0;

	for (i = 0; i < vfExec->len; i++)
		if (vfExec->data[i] == 0)
			count++;

	return count;
}

static void bn_set_int(BIGNUM *n, int val)
{
	if (val >= 0)
		BN_set_word(n, val);
	else {
		BN_set_word(n, -val);
		BN_set_negative(n, 1);
	}
}

bool bp_script_eval(GPtrArray *stack, const GString *script,
		    const struct bp_tx *txTo, unsigned int nIn,
		    int nHashType)
{
	struct const_buffer pc = { script->str, script->len };
	struct const_buffer pend = { script->str + script->len, 0 };
	struct const_buffer pbegincodehash = { script->str, script->len };
	struct bscript_op op;
	bool rc = false;
	GByteArray *vfExec = g_byte_array_new();
	GPtrArray *altstack = g_ptr_array_new_with_free_func(
						(GDestroyNotify) buffer_free);
	BIGNUM bn;
	BN_init(&bn);

	if (script->len > 10000)
		goto out;
	
	unsigned int nOpCount = 0;

	struct bscript_parser bp;
	bsp_start(&bp, &pc);

	while (pc.p < pend.p) {
		bool fExec = !count_false(vfExec);

		if (!bsp_getop(&op, &bp))
			goto out;
		enum opcodetype opcode = op.op;

		if (op.data.len > 520)
			goto out;
		if (opcode > OP_16 && ++nOpCount > 201)
			goto out;
		if (disabled_op[opcode])
			goto out;

		if (fExec && is_bsp_pushdata(opcode))
			stack_push(stack, (struct buffer *) &op.data);
		else if (fExec || (OP_IF <= opcode && opcode <= OP_ENDIF))
		switch (opcode) {

		//
		// Push value
		//
		case OP_1NEGATE:
		case OP_1:
		case OP_2:
		case OP_3:
		case OP_4:
		case OP_5:
		case OP_6:
		case OP_7:
		case OP_8:
		case OP_9:
		case OP_10:
		case OP_11:
		case OP_12:
		case OP_13:
		case OP_14:
		case OP_15:
		case OP_16:
			bn_set_int(&bn, (int)opcode - (int)(OP_1 - 1));
			stack_push_str(stack, bn_getvch(&bn));
			break;

		//
		// Control
		//
		case OP_NOP:
		case OP_NOP1: case OP_NOP2: case OP_NOP3: case OP_NOP4: case
OP_NOP5:
		case OP_NOP6: case OP_NOP7: case OP_NOP8: case OP_NOP9: case
OP_NOP10:
			break;

		case OP_IF:
		case OP_NOTIF: {
			// <expression> if [statements] [else [statements]] endif
			bool fValue = false;
			if (fExec) {
				if (stack->len < 1)
					goto out;
				struct buffer *vch = stacktop(stack, -1);
				fValue = CastToBool(vch);
				if (opcode == OP_NOTIF)
					fValue = !fValue;
				popstack(stack);
			}
			guint8 vc = (guint8) fValue;
			g_byte_array_append(vfExec, &vc, 1);
			break;
		}

		case OP_ELSE: {
			if (vfExec->len == 0)
				goto out;
			guint8 *v = &vfExec->data[vfExec->len - 1];
			*v = !(*v);
			break;
		}

		case OP_ENDIF:
			if (vfExec->len == 0)
				goto out;
			g_byte_array_remove_index(vfExec, vfExec->len - 1);
			break;

		case OP_VERIFY: {
			if (stack->len < 1)
				goto out;
			bool fValue = CastToBool(stacktop(stack, -1));
			if (fValue)
				popstack(stack);
			else
				goto out;
			break;
		}

		case OP_RETURN:
			goto out;

		//
		// Stack ops
		//
		case OP_TOALTSTACK:
			if (stack->len < 1)
				goto out;
			stack_push(altstack, stacktop(stack, -1));
			popstack(stack);
			break;

		case OP_FROMALTSTACK:
			if (altstack->len < 1)
				goto out;
			stack_push(stack, stacktop(altstack, -1));
			popstack(altstack);
			break;

		case OP_2DROP:
			// (x1 x2 -- )
			if (stack->len < 2)
				goto out;
			popstack(stack);
			popstack(stack);
			break;

		case OP_2DUP: {
			// (x1 x2 -- x1 x2 x1 x2)
			if (stack->len < 2)
				goto out;
			struct buffer *vch1 = stacktop(stack, -2);
			struct buffer *vch2 = stacktop(stack, -1);
			stack_push(stack, vch1);
			stack_push(stack, vch2);
			break;
		}

		case OP_3DUP: {
			// (x1 x2 x3 -- x1 x2 x3 x1 x2 x3)
			if (stack->len < 3)
				goto out;
			struct buffer *vch1 = stacktop(stack, -3);
			struct buffer *vch2 = stacktop(stack, -2);
			struct buffer *vch3 = stacktop(stack, -1);
			stack_push(stack, vch1);
			stack_push(stack, vch2);
			stack_push(stack, vch3);
			break;
		}

		case OP_2OVER: {
			// (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
			if (stack->len < 4)
				goto out;
			struct buffer *vch1 = stacktop(stack, -4);
			struct buffer *vch2 = stacktop(stack, -3);
			stack_push(stack, vch1);
			stack_push(stack, vch2);
			break;
		}

		case OP_2ROT: {
			// (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
			if (stack->len < 6)
				goto out;
			struct buffer *vch1 = stack_take(stack, -6);
			struct buffer *vch2 = stack_take(stack, -5);
			g_ptr_array_remove_range(stack, stack->len - 6, 2);
			stack_push(stack, vch1);
			stack_push(stack, vch2);
			break;
		}

		case OP_2SWAP:
			// (x1 x2 x3 x4 -- x3 x4 x1 x2)
			if (stack->len < 4)
				goto out;
			stack_swap(stack, -4, -2);
			stack_swap(stack, -3, -1);
			break;

		case OP_IFDUP: {
			// (x - 0 | x x)
			if (stack->len < 1)
				goto out;
			struct buffer *vch = stacktop(stack, -1);
			if (CastToBool(vch))
				stack_push(stack, vch);
			break;
		}

		case OP_DEPTH:
			// -- stacksize
			BN_set_word(&bn, stack->len);
			stack_push_str(stack, bn_getvch(&bn));
			break;

		case OP_DROP:
			// (x -- )
			if (stack->len < 1)
				goto out;
			popstack(stack);
			break;

		case OP_DUP: {
			// (x -- x x)
			if (stack->len < 1)
				goto out;
			struct buffer *vch = stacktop(stack, -1);
			stack_push(stack, vch);
			break;
		}

		case OP_NIP:
			// (x1 x2 -- x2)
			if (stack->len < 2)
				goto out;
			g_ptr_array_remove_index(stack, stack->len - 2);
			break;

		case OP_OVER: {
			// (x1 x2 -- x1 x2 x1)
			if (stack->len < 2)
				goto out;
			struct buffer *vch = stacktop(stack, -2);
			stack_push(stack, vch);
			break;
		}

#if 0 /* current work pointer; script ops below this point need work */
		case OP_PICK:
		case OP_ROLL: {
			// (xn ... x2 x1 x0 n - xn ... x2 x1 x0 xn)
			// (xn ... x2 x1 x0 n - ... x2 x1 x0 xn)
			if (stack->len < 2)
				goto out;
			int n = CastToBigNum(stacktop(stack, -1)).getint();
			popstack(stack);
			if (n < 0 || n >= (int)stack->len)
				goto out;
			struct buffer *vch = stacktop(stack, -n-1);
			if (opcode == OP_ROLL)
				stack.erase(stack.end()-n-1);
			stack_push(stack, vch);
			break;
		}
#endif

		case OP_ROT: {
			// (x1 x2 x3 -- x2 x3 x1)
			//  x2 x1 x3  after first swap
			//  x2 x3 x1  after second swap
			if (stack->len < 3)
				goto out;
			stack_swap(stack, -3, -2);
			stack_swap(stack, -2, -1);
			break;
		}

		case OP_SWAP: {
			// (x1 x2 -- x2 x1)
			if (stack->len < 2)
				goto out;
			stack_swap(stack, -2, -1);
			break;
		}

#if 0
		case OP_TUCK: {
			// (x1 x2 -- x2 x1 x2)
			if (stack->len < 2)
				goto out;
			struct buffer *vch = stacktop(stack, -1);
			stack.insert(stack.end()-2, vch);
			break;
		}


		//
		// Splice ops
		//
		case OP_CAT: {
			// (x1 x2 -- out)
			if (stack->len < 2)
				goto out;
			struct buffer *vch1 = stacktop(stack, -2);
			struct buffer *vch2 = stacktop(stack, -1);
			vch1.insert(vch1.end(), vch2.begin(), vch2.end());
			popstack(stack);
			if (stacktop(stack, -1).size() > 520)
				goto out;
			break;
		}

		case OP_SUBSTR: {
			// (in begin size -- out)
			if (stack->len < 3)
				goto out;
			struct buffer *vch = stacktop(stack, -3);
			int nBegin = CastToBigNum(stacktop(stack, -2)).getint();
			int nEnd = nBegin + CastToBigNum(stacktop(stack, -1)).getint();
			if (nBegin < 0 || nEnd < nBegin)
				goto out;
			if (nBegin > (int)vch.size())
				nBegin = vch.size();
			if (nEnd > (int)vch.size())
				nEnd = vch.size();
			vch.erase(vch.begin() + nEnd, vch.end());
			vch.erase(vch.begin(), vch.begin() + nBegin);
			popstack(stack);
			popstack(stack);
			break;
		}

		case OP_LEFT:
		case OP_RIGHT: {
			// (in size -- out)
			if (stack->len < 2)
				goto out;
			struct buffer *vch = stacktop(stack, -2);
			int nSize = CastToBigNum(stacktop(stack, -1)).getint();
			if (nSize < 0)
				goto out;
			if (nSize > (int)vch.size())
				nSize = vch.size();
			if (opcode == OP_LEFT)
				vch.erase(vch.begin() + nSize, vch.end());
			else
				vch.erase(vch.begin(), vch.end() - nSize);
			popstack(stack);
			break;
		}
#endif

		case OP_SIZE: {
			// (in -- in size)
			if (stack->len < 1)
				goto out;
			struct buffer *vch = stacktop(stack, -1);
			BN_set_word(&bn, vch->len);
			stack_push_str(stack, bn_getvch(&bn));
			break;
		}


		//
		// Bitwise logic
		//
		case OP_INVERT: {
			// (in - out)
			if (stack->len < 1)
				goto out;
			struct buffer *vch_ = stacktop(stack, -1);
			unsigned char *vch = vch_->p;
			unsigned int i;
			for (i = 0; i < vch_->len; i++)
				vch[i] = ~vch[i];
			break;
		}

#if 0
		//
		// WARNING: These disabled opcodes exhibit unexpected behavior
		// when used on signed integers due to a bug in MakeSameSize()
		// [see definition of MakeSameSize() above].
		//
		case OP_AND:
		case OP_OR:
		case OP_XOR: {
			// (x1 x2 - out)
			if (stack->len < 2)
				goto out;
			struct buffer *vch1 = stacktop(stack, -2);
			struct buffer *vch2 = stacktop(stack, -1);
			MakeSameSize(vch1, vch2); // <-- NOT SAFE FOR SIGNED VALUES
			if (opcode == OP_AND)
			{
				for (unsigned int i = 0; i < vch1.size(); i++)
					vch1[i] &= vch2[i];
			}
			else if (opcode == OP_OR)
			{
				for (unsigned int i = 0; i < vch1.size(); i++)
					vch1[i] |= vch2[i];
			}
			else if (opcode == OP_XOR)
			{
				for (unsigned int i = 0; i < vch1.size(); i++)
					vch1[i] ^= vch2[i];
			}
			popstack(stack);
			break;
		}
#endif

		case OP_EQUAL:
		case OP_EQUALVERIFY: {
			// (x1 x2 - bool)
			if (stack->len < 2)
				goto out;
			struct buffer *vch1 = stacktop(stack, -2);
			struct buffer *vch2 = stacktop(stack, -1);
			bool fEqual = ((vch1->len == vch2->len) &&
				      memcmp(vch1->p, vch2->p, vch1->len) == 0);
			// OP_NOTEQUAL is disabled because it would be too easy to say
			// something like n != 1 and have some wiseguy pass in 1 with extra
			// zero bytes after it (numerically, 0x01 == 0x0001 == 0x000001)
			//if (opcode == OP_NOTEQUAL)
			//	fEqual = !fEqual;
			popstack(stack);
			popstack(stack);
			stack_push_char(stack, fEqual ? 1 : 0);
			if (opcode == OP_EQUALVERIFY) {
				if (fEqual)
					popstack(stack);
				else
					goto out;
			}
			break;
		}

		//
		// Numeric
		//
		case OP_1ADD:
		case OP_1SUB:
		case OP_2MUL:
		case OP_2DIV:
		case OP_NEGATE:
		case OP_ABS:
		case OP_NOT:
		case OP_0NOTEQUAL: {
			// (in -- out)
			if (stack->len < 1)
				goto out;
			if (!CastToBigNum(&bn, stacktop(stack, -1)))
				goto out;
			switch (opcode)
			{
			case OP_1ADD:
				BN_add_word(&bn, 1);
				break;
			case OP_1SUB:
				BN_sub_word(&bn, 1);
				break;
			case OP_2MUL:
				BN_lshift1(&bn, &bn);
				break;
			case OP_2DIV:
				BN_rshift1(&bn, &bn);
				break;
			case OP_NEGATE:
				BN_set_negative(&bn, !BN_is_negative(&bn));
				break;
			case OP_ABS:
				if (BN_is_negative(&bn))
					BN_set_negative(&bn, 0);
				break;
			case OP_NOT:
				BN_set_word(&bn, BN_is_zero(&bn) ? 1 : 0);
				break;
			case OP_0NOTEQUAL:
				BN_set_word(&bn, BN_is_zero(&bn) ? 0 : 1);
				break;
			default:
				// impossible
				goto out;
			}
			popstack(stack);
			stack_push_str(stack, bn_getvch(&bn));
			break;
		}

#if 0
		case OP_ADD:
		case OP_SUB:
		case OP_MUL:
		case OP_DIV:
		case OP_MOD:
		case OP_LSHIFT:
		case OP_RSHIFT:
		case OP_BOOLAND:
		case OP_BOOLOR:
		case OP_NUMEQUAL:
		case OP_NUMEQUALVERIFY:
		case OP_NUMNOTEQUAL:
		case OP_LESSTHAN:
		case OP_GREATERTHAN:
		case OP_LESSTHANOREQUAL:
		case OP_GREATERTHANOREQUAL:
		case OP_MIN:
		case OP_MAX: {
			// (x1 x2 -- out)
			if (stack->len < 2)
				goto out;
			CBigNum bn1 = CastToBigNum(stacktop(stack, -2));
			CBigNum bn2 = CastToBigNum(stacktop(stack, -1));
			CBigNum bn;
			switch (opcode)
			{
			case OP_ADD:
				bn = bn1 + bn2;
				break;

			case OP_SUB:
				bn = bn1 - bn2;
				break;

			case OP_MUL:
				if (!BN_mul(&bn, &bn1, &bn2, pctx))
					goto out;
				break;

			case OP_DIV:
				if (!BN_div(&bn, NULL, &bn1, &bn2, pctx))
					goto out;
				break;

			case OP_MOD:
				if (!BN_mod(&bn, &bn1, &bn2, pctx))
					goto out;
				break;

			case OP_LSHIFT:
				if (bn2 < bnZero || bn2 > CBigNum(2048))
					goto out;
				bn = bn1 << bn2.getulong();
				break;

			case OP_RSHIFT:
				if (bn2 < bnZero || bn2 > CBigNum(2048))
					goto out;
				bn = bn1 >> bn2.getulong();
				break;

			case OP_BOOLAND:			 bn = (bn1 != bnZero && bn2 != bnZero); break;
			case OP_BOOLOR:			  bn = (bn1 != bnZero || bn2 != bnZero); break;
			case OP_NUMEQUAL:			bn = (bn1 == bn2); break;
			case OP_NUMEQUALVERIFY:	  bn = (bn1 == bn2); break;
			case OP_NUMNOTEQUAL:		 bn = (bn1 != bn2); break;
			case OP_LESSTHAN:			bn = (bn1 < bn2); break;
			case OP_GREATERTHAN:		 bn = (bn1 > bn2); break;
			case OP_LESSTHANOREQUAL:	 bn = (bn1 <= bn2); break;
			case OP_GREATERTHANOREQUAL:  bn = (bn1 >= bn2); break;
			case OP_MIN:		 bn = (bn1 < bn2 ? bn1 : bn2); break;
			case OP_MAX:		 bn = (bn1 > bn2 ? bn1 : bn2); break;
			default:			 assert(!"invalid opcode"); break;
			}
			popstack(stack);
			popstack(stack);
			stack_push(stack, bn.getvch());

			if (opcode == OP_NUMEQUALVERIFY)
			{
				if (CastToBool(stacktop(stack, -1)))
					popstack(stack);
				else
					goto out;
			}
			break;
		}
#endif

		case OP_WITHIN: {
			// (x min max -- out)
			if (stack->len < 3)
				goto out;
			BIGNUM bn1, bn2, bn3;
			BN_init(&bn1);
			BN_init(&bn2);
			BN_init(&bn3);
			bool rc1 = CastToBigNum(&bn1, stacktop(stack, -3));
			bool rc2 = CastToBigNum(&bn2, stacktop(stack, -2));
			bool rc3 = CastToBigNum(&bn3, stacktop(stack, -1));
			bool fValue = (BN_cmp(&bn2, &bn1) <= 0 &&
				       BN_cmp(&bn1, &bn3) < 0);
			popstack(stack);
			popstack(stack);
			popstack(stack);
			stack_push_char(stack, fValue ? 1 : 0);
			BN_clear_free(&bn1);
			BN_clear_free(&bn2);
			BN_clear_free(&bn3);
			if (!rc1 || !rc2 || !rc3)
				goto out;
			break;
		}

#if 0
		//
		// Crypto
		//
		case OP_RIPEMD160:
		case OP_SHA1:
		case OP_SHA256:
		case OP_HASH160:
		case OP_HASH256: {
			// (in -- hash)
			if (stack->len < 1)
				goto out;
			struct buffer *vch = stacktop(stack, -1);
			struct buffer *vchHash((opcode == OP_RIPEMD160 || opcode == OP_SHA1 || opcode == OP_HASH160) ? 20 : 32);
			if (opcode == OP_RIPEMD160)
				RIPEMD160(&vch[0], vch.size(), &vchHash[0]);
			else if (opcode == OP_SHA1)
				SHA1(&vch[0], vch.size(), &vchHash[0]);
			else if (opcode == OP_SHA256)
				SHA256(&vch[0], vch.size(), &vchHash[0]);
			else if (opcode == OP_HASH160)
			{
				uint160 hash160 = Hash160(vch);
				memcpy(&vchHash[0], &hash160, sizeof(hash160));
			}
			else if (opcode == OP_HASH256)
			{
				uint256 hash = Hash(vch.begin(), vch.end());
				memcpy(&vchHash[0], &hash, sizeof(hash));
			}
			popstack(stack);
			stack_push(stack, vchHash);
			break;
		}
#endif

		case OP_CODESEPARATOR:
			// Hash starts after the code separator
			memcpy(&pbegincodehash, &pc, sizeof(pc));
			break;

#if 0
		case OP_CHECKSIG:
		case OP_CHECKSIGVERIFY: {
			// (sig pubkey -- bool)
			if (stack->len < 2)
				goto out;

			struct buffer *vchSig	= stacktop(stack, -2);
			struct buffer *vchPubKey = stacktop(stack, -1);

			////// debug print
			//PrintHex(vchSig.begin(), vchSig.end(), "sig: %s\n");
			//PrintHex(vchPubKey.begin(), vchPubKey.end(), "pubkey: %s\n");

			// Subset of script starting at the most recent codeseparator
			CScript scriptCode(pbegincodehash, pend);

			// Drop the signature, since there's no way for a signature to sign itself
			scriptCode.FindAndDelete(CScript(vchSig));

			bool fSuccess = (!fStrictEncodings || (IsCanonicalSignature(vchSig) && IsCanonicalPubKey(vchPubKey)));
			if (fSuccess)
				fSuccess = CheckSig(vchSig, vchPubKey, scriptCode, txTo, nIn, nHashType);

			popstack(stack);
			popstack(stack);
			stack_push(stack, fSuccess ? vchTrue : vchFalse);
			if (opcode == OP_CHECKSIGVERIFY)
			{
				if (fSuccess)
					popstack(stack);
				else
					goto out;
			}
			break;
		}

		case OP_CHECKMULTISIG:
		case OP_CHECKMULTISIGVERIFY: {
			// ([sig ...] num_of_signatures [pubkey ...] num_of_pubkeys -- bool)

			int i = 1;
			if ((int)stack->len < i)
				goto out;

			int nKeysCount = CastToBigNum(stacktop(stack, -i)).getint();
			if (nKeysCount < 0 || nKeysCount > 20)
				goto out;
			nOpCount += nKeysCount;
			if (nOpCount > 201)
				goto out;
			int ikey = ++i;
			i += nKeysCount;
			if ((int)stack->len < i)
				goto out;

			int nSigsCount = CastToBigNum(stacktop(stack, -i)).getint();
			if (nSigsCount < 0 || nSigsCount > nKeysCount)
				goto out;
			int isig = ++i;
			i += nSigsCount;
			if ((int)stack->len < i)
				goto out;

			// Subset of script starting at the most recent codeseparator
			CScript scriptCode(pbegincodehash, pend);

			// Drop the signatures, since there's no way for a signature to sign itself
			for (int k = 0; k < nSigsCount; k++)
			{
				struct buffer *vchSig = stacktop(stack, -isig-k);
				scriptCode.FindAndDelete(CScript(vchSig));
			}

			bool fSuccess = true;
			while (fSuccess && nSigsCount > 0)
			{
				struct buffer *vchSig	= stacktop(stack, -isig);
				struct buffer *vchPubKey = stacktop(stack, -ikey);

				// Check signature
				bool fOk = (!fStrictEncodings || (IsCanonicalSignature(vchSig) && IsCanonicalPubKey(vchPubKey)));
				if (fOk)
					fOk = CheckSig(vchSig, vchPubKey, scriptCode, txTo, nIn, nHashType);

				if (fOk) {
					isig++;
					nSigsCount--;
				}
				ikey++;
				nKeysCount--;

				// If there are more signatures left than keys left,
				// then too many signatures have failed
				if (nSigsCount > nKeysCount)
					fSuccess = false;
			}

			while (i-- > 0)
				popstack(stack);
			stack_push(stack, fSuccess ? vchTrue : vchFalse);

			if (opcode == OP_CHECKMULTISIGVERIFY)
			{
				if (fSuccess)
					popstack(stack);
				else
					goto out;
			}
			break;
		}
#endif

		default:
			goto out;
		}

		if (stack->len + altstack->len > 1000)
			goto out;
	}

	rc = (vfExec->len == 0 && bp.error == false);

out:
	BN_clear_free(&bn);
	g_ptr_array_free(altstack, TRUE);
	g_byte_array_unref(vfExec);
	return rc;
}

