/*
	Driver for NXP Semiconductors ISDB-S tuner TDA2014x

	Copyright (C) Budi Rachmanto, AreMa Inc. <info@are.ma>
*/

#include <media/dvb_frontend.h>
#include "tda2014x.h"

static int tda2014x_r(struct i2c_client *c, u8 slvadr)
{
	u8	buf[]	= {0xFE, 0xA8, slvadr},
		rcmd[]	= {0xFE, 0xA9},
		ret	= 0;
	struct i2c_msg msg[] = {
		{.addr = c->addr,		.flags = 0,		.buf = buf,	.len = 3,},
		{.addr = 0x80 | c->addr,	.flags = 0,		.buf = rcmd,	.len = 2,},
		{.addr = c->addr,		.flags = I2C_M_RD,	.buf = &ret,	.len = 1,},
	};
	return i2c_transfer(c->adapter, msg, 3) == 3 ? ret : -EREMOTEIO;
}

static bool tda2014x_r8(struct i2c_client *c, u16 slvadr, u8 start_bit, u8 nbits, u8 *rdat)
{
	u8	mask	= nbits > 7 ? 0xFF : ((1 << nbits) - 1) << start_bit,
		val	= tda2014x_r(c, slvadr);

	if (val < 0)
		return false;
	*rdat = (val & mask) >> start_bit;
	return true;
}

static bool tda2014x_w8(struct i2c_client *c, u8 slvadr, u8 dat)	// tc90522_i2c_w_tuner
{
	u8		buf[]	= {slvadr, dat};
	struct i2c_msg	msg[]	= {
		{.addr = c->addr,	.flags = 0,	.buf = buf,	.len = 2,},
	};
	return i2c_transfer(c->adapter, msg, 1) == 1;
}

static bool tda2014x_w16(struct i2c_client *c, u16 slvadr, u8 start_bit, u8 nbits, u8 nbytes, bool rmw, u8 access, u16 wdat)
{
	u16	mask	= nbits > 15 ? 0xFFFF : ((1 << nbits) - 1) << start_bit,
		val	= mask & (wdat << start_bit);
	u8	*wval	= (u8 *)&val,
		i;

	for (i = 0, nbytes = !nbytes ? 1 : nbytes > 2 ? 2 : nbytes; access & 2 && nbytes; i++, nbytes--) {
		u8	buf[]	= {0xFE, 0xA8, slvadr + i, 0},
			ret	= tda2014x_r(c, slvadr + i);
		struct i2c_msg msg[] = {
			{.addr = c->addr,	.flags = 0,	.buf = buf,	.len = 4,},
		};

		if (ret < 0)
			return false;
		if (rmw)
			wval[nbytes - 1] |= ~(mask >> 8 * i) & ret;
		buf[3] = wval[nbytes - 1];

		if (i2c_transfer(c->adapter, msg, 1) != 1)
			return false;
	}
	return true;
}

static int tda2014x_tune(struct dvb_frontend *fe)
{
	u64 div10(u64 n, u8 pow)
	{
		u64	q, r;

		while (pow--) {
			q = (n >> 1) + (n >> 2);
			q = q + (q >> 4);
			q = q + (q >> 8);
			q = q + (q >> 16);
			q = q >> 3;
			r = n - (((q << 2) + q) << 1);
			n = q + (r > 9);
		}
		return n;
	}

	enum {
		TDA2014X_LNA_GAIN_7dB	= 0x0,
		TDA2014X_LNA_GAIN_10dB	= 0x1,
		TDA2014X_LNA_GAIN_13dB	= 0x2,		/* default */
		TDA2014X_LNA_GAIN_18dB	= 0x3,
		TDA2014X_LNA_GAIN_NEGATIVE_11dB = 0x4,

		TDA2014X_LPT_GAIN_NEGATIVE_8dB	= 0x0,
		TDA2014X_LPT_GAIN_NEGATIVE_10dB	= 0x1,	/* default */
		TDA2014X_LPT_GAIN_NEGATIVE_14dB	= 0x2,
		TDA2014X_LPT_GAIN_NEGATIVE_16dB	= 0x3,

		TDA2014X_AMPOUT_15DB	= 0x0,
		TDA2014X_AMPOUT_18DB	= 0x1,
		TDA2014X_AMPOUT_18DB_b	= 0x2,
		TDA2014X_AMPOUT_21DB	= 0x3,		/* default */
		TDA2014X_AMPOUT_24DB	= 0x6,		/* ok too */
		TDA2014X_AMPOUT_27DB	= 0x7,
		TDA2014X_AMPOUT_30DB	= 0xE,
		TDA2014X_AMPOUT_33DB	= 0xF,
	};
	bool	bDoublerEnable[]		= {false, true, true, true, true},
		bDcc1Enable[]			= {false, true, true, true, true},
		bDcc2Enable[]			= {false, true, true, true, true},
		bPpfEnable[]			= {false, true, true, true, true},
		bDiv1ConfigInDivideBy3[]	= {false, true, false, true, false},
		bDiv2ConfigInDivideBy3[]	= {false, true, true, false, false},
		bSelectDivideBy4Or5Or6Or7Path[]	= {false, true, true, true, true},
		bSelectDivideBy8Path[]		= {true, false, false, false, false},
		bInputMuxEnable;
	u8	PredividerRatio,
		val;
	u32	f_kHz = fe->dtv_property_cache.frequency;
	u64	ResLsb,
		CalcPow = 6,
		Premain,
		R,
		kint,
		Nint,
		DsmFracInReg,
		DsmIntInReg,
		v15;
	int	ePllRefClkRatio,
		i = f_kHz <= 1075000 ? 0 : f_kHz <= 1228000 ? 1 : f_kHz <= 1433000 ? 2 : f_kHz <= 1720000 ? 3 : 4,
		lna	= TDA2014X_LNA_GAIN_13dB,
		gain	= (lna == TDA2014X_LNA_GAIN_18dB) | ((lna & 3) << 4) | (TDA2014X_LPT_GAIN_NEGATIVE_10dB << 1),
		ampout	= TDA2014X_AMPOUT_21DB;
	struct i2c_client	*c	= fe->tuner_priv;

	/* GetLoConfig */
	if (!tda2014x_r8(c, 0x25, 3, 1, &val))
		return -EIO;
	bInputMuxEnable = val;

	/* SetLoConfig */
	if (tda2014x_w16(c, 0x22, 0, 8, 0, 0, 6,
		(bDoublerEnable[i] << 7) | (bDcc1Enable[i] << 6) | (bDcc2Enable[i] << 5) | 0b11110 | bPpfEnable[i]) &&
		tda2014x_r8(c, 0x23, 0, 8, &val) &&
		tda2014x_w16(c, 0x23, 0, 8, 0, 0, 6, (bDiv1ConfigInDivideBy3[i] << 7) | (bDiv2ConfigInDivideBy3[i] << 5) |
			(bSelectDivideBy4Or5Or6Or7Path[i] << 3) | (bSelectDivideBy8Path[i] << 2) | (val & 0b1010011)))
		tda2014x_w16(c, 0x25, 3, 1, 0, 1, 6, bInputMuxEnable);

	ResLsb = (8 - i) * f_kHz * 1000UL / 27UL;	/* Xtal 27 MHz */
	kint = ResLsb;
	v15 = div10(ResLsb, 6);
	R = 1;
	Premain = 1;
	Nint = (v15 * R) >> Premain;
	if (Nint < 131) {
		Premain = 0;
		Nint = (v15 * R) >> Premain;
		if (Nint > 251) {
			R = 3;
			Premain = 2;
			goto LABEL_36;
		}
		if (Nint < 131) {
			R = 3;
			Premain = 1;
			goto LABEL_36;
		}
	} else if (Nint > 251) {
		Premain = 2;
		Nint = (v15 * R) >> Premain;
		if (Nint > 251) {
			R = 3;
			Premain = 2;
		}
LABEL_36:
		Nint = (v15 * R) >> Premain;
		if (Nint < 131 || Nint > 251)
			return -ERANGE;
	}
	switch ((100 * R) >> Premain) {
	case 25:
		kint = ResLsb / 4;
		break;
	case 50:
		kint = ResLsb / 2;
		break;
	case 75:
		kint = ResLsb / 2 + ResLsb / 4;
		break;
	case 100:
		break;
	case 150:
		kint = ResLsb / 2 + ResLsb;
		break;
	default:
		return -ERANGE;
	}
	kint		= div10(kint, 1) * 10;
	ePllRefClkRatio	= R == 2 ? 1 : R == 3 ? 2 : 0;
	PredividerRatio	= Premain == 1 ? 0 : 1;
	DsmIntInReg	= div10(kint, 6);
	DsmFracInReg	= kint - 1000000 * DsmIntInReg;
	for (i = 0; i < 16; i++) {
		DsmFracInReg *= 2;
		if (DsmFracInReg > 0xFFFFFFF && i != 15) {
			DsmFracInReg = div10(DsmFracInReg, 1);
			CalcPow--;
		}
	}
	return	!(tda2014x_w8(c, 0xA, 0)	&&
		tda2014x_w8(c, 0x10, 0xB0)	&&
		tda2014x_w8(c, 0x11, 2)	&&
		tda2014x_w8(c, 3, 1)		&&

		tda2014x_w16(c, 3, 6, 2, 0, 1, 6, ePllRefClkRatio)	&&

		/* SetPllDividerConfig */
		tda2014x_w16(c, 0x1A, 5, 1, 0, 1, 6, PredividerRatio)			&&
		tda2014x_w16(c, 0x1E, 0, 8, 0, 0, 6, DsmIntInReg - 128)		&&
		tda2014x_w16(c, 0x1F, 0, 0x10, 2, 0, 6, div10(DsmFracInReg, CalcPow))	&&

		/* ProgramVcoChannelChange */
		tda2014x_r8(c, 0x12, 0, 8, &val)				&&
		tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, (val & 0x7F) | 0x40)	&&
		tda2014x_w16(c, 0x13, 0, 2, 0, 1, 6, 2)			&&
		tda2014x_w16(c, 0x13, 7, 1, 0, 1, 6, 0)			&&
		tda2014x_w16(c, 0x13, 5, 1, 0, 1, 6, 1)			&&
		tda2014x_w16(c, 0x13, 5, 1, 0, 1, 6, 1)			&&
		tda2014x_w16(c, 0x13, 7, 1, 0, 1, 6, 0)			&&
		tda2014x_w16(c, 0x13, 4, 1, 0, 1, 6, 1)			&&
		((tda2014x_r8(c, 0x15, 4, 1, &val) && val == 1)		||
		(tda2014x_r8(c, 0x15, 4, 1, &val) && val == 1))		&&
		tda2014x_w16(c, 0x13, 4, 1, 0, 1, 6, 0)			&&
		tda2014x_r8(c, 0x12, 0, 8, &val)				&&
		tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, val & 0x7F)		&&

		/* SetFilterBandwidth */
		tda2014x_w16(c, 0xA, 0, 4, 0, 1, 6, 0xA)	&&
		tda2014x_w16(c, 0xB, 1, 7, 0, 1, 6, 0x7C)	&&

		/* SetGainConfig */
		tda2014x_r8(c, 6, 0, 8, &val)					&&
		tda2014x_w16(c, 6, 0, 8, 0, 0, 6, (val & 0x48) | 0x80 | gain)	&&
		tda2014x_r8(c, 9, 0, 8, &val)					&&
		tda2014x_w16(c, 9, 0, 8, 0, 0, 6, 0b10110000 | (val & 3))	&&
		tda2014x_w16(c, 0xA, 5, 3, 0, 1, 6, 3)				&&
		tda2014x_w16(c, 0xC, 4, 4, 0, 1, 6, ampout)			&&

		tda2014x_w8(c, 0xA, 0xFF)	&&
		tda2014x_w8(c, 0x10, 0xB2)	&&
		tda2014x_w8(c, 0x11, 0)	&&
		tda2014x_w8(c, 3, 1)) * -EIO;
}

static int tda2014x_probe(struct i2c_client *c)
{
	u8			val	= 0;
	struct dvb_frontend	*fe	= c->dev.platform_data;

	fe->tuner_priv			= c;
	fe->ops.tuner_ops.set_params	= tda2014x_tune;
	fe->dtv_property_cache.frequency = 1318000;
	return	!(tda2014x_w8(c, 0x13, 0)	&&
		tda2014x_w8(c, 0x15, 0)	&&
		tda2014x_w8(c, 0x17, 0)	&&
		tda2014x_w8(c, 0x1C, 0)	&&
		tda2014x_w8(c, 0x1D, 0)	&&
		tda2014x_w8(c, 0x1F, 0)	&&
		(tda2014x_w8(c, 7, 0x31),
		 tda2014x_w8(c, 8, 0x77),
		 tda2014x_w8(c, 4, 2))		&&

		/* SetPowerMode */
		tda2014x_r8(c, 2, 0, 8, &val)					&&
		tda2014x_w16(c, 2, 0, 8, 0, 0, 6, val | 0x81)			&&
		tda2014x_r8(c, 6, 0, 8, &val)					&&
		tda2014x_w16(c, 6, 0, 8, 0, 0, 6, (val | 0x39) & 0x7F)		&&
		tda2014x_r8(c, 7, 0, 8, &val)					&&
		tda2014x_w16(c, 7, 0, 8, 0, 0, 6, val | 0xAE)			&&
		tda2014x_r8(c, 0xF, 0, 8, &val)				&&
		tda2014x_w16(c, 0xF, 0, 8, 0, 0, 6, val | 0x80)		&&
		tda2014x_r8(c, 0x18, 0, 8, &val)				&&
		tda2014x_w16(c, 0x18, 0, 8, 0, 0, 6, val & 0x7F)		&&
		tda2014x_r8(c, 0x1A, 0, 8, &val)				&&
		tda2014x_w16(c, 0x1A, 0, 8, 0, 0, 6, val | 0xC0)		&&
		tda2014x_w16(c, 0x22, 0, 8, 0, 0, 6, 0xFF)			&&
		tda2014x_r8(c, 0x23, 0, 8, &val)				&&
		tda2014x_w16(c, 0x23, 0, 8, 0, 0, 6, val & 0xFE)		&&
		tda2014x_r8(c, 0x25, 0, 8, &val)				&&
		tda2014x_w16(c, 0x25, 0, 8, 0, 0, 6, val | 8)			&&
		tda2014x_r8(c, 0x27, 0, 8, &val)				&&
		tda2014x_w16(c, 0x27, 0, 8, 0, 0, 6, (val | 0xC0) & 0xDF)	&&
		tda2014x_r8(c, 0x24, 0, 8, &val)				&&
		tda2014x_w16(c, 0x24, 0, 8, 0, 0, 6, (val | 4) & 0xCF)		&&
		tda2014x_r8(c, 0xD, 0, 8, &val)				&&
		tda2014x_w16(c, 0xD, 0, 8, 0, 0, 6, val & 0xDF)		&&
		tda2014x_r8(c, 9, 0, 8, &val)					&&
		tda2014x_w16(c, 9, 0, 8, 0, 0, 6, (val | 0xB0) & 0xB1)		&&
		tda2014x_r8(c, 0xA, 0, 8, &val)				&&
		tda2014x_w16(c, 0xA, 0, 8, 0, 0, 6, (val | 0x6F) & 0x7F)	&&
		tda2014x_r8(c, 0xB, 0, 8, &val)				&&
		tda2014x_w16(c, 0xB, 0, 8, 0, 0, 6, (val | 0x7A) & 0x7B)	&&
		tda2014x_w16(c, 0xC, 0, 8, 0, 0, 6, 0)				&&
		tda2014x_w16(c, 0x19, 0, 8, 0, 0, 6, 0xFA)			&&
		tda2014x_r8(c, 0x1B, 0, 8, &val)				&&
		tda2014x_w16(c, 0x1B, 0, 8, 0, 0, 6, val & 0x7F)		&&
		tda2014x_r8(c, 0x21, 0, 8, &val)				&&
		tda2014x_w16(c, 0x21, 0, 8, 0, 0, 6, val | 0x40)		&&
		tda2014x_r8(c, 0x10, 0, 8, &val)				&&
		tda2014x_w16(c, 0x10, 0, 8, 0, 0, 6, (val | 0x90) & 0xBF)	&&
		tda2014x_r8(c, 0x14, 0, 8, &val)				&&
		tda2014x_w16(c, 0x14, 0, 8, 0, 0, 6, (val | 0x20) & 0xEF)	&&

		/* ProgramPllPor */
		tda2014x_w16(c, 0x1A, 6, 1, 0, 1, 6, 1)	&&
		tda2014x_w16(c, 0x18, 0, 1, 0, 1, 6, 1)	&&
		tda2014x_w16(c, 0x18, 7, 1, 0, 1, 6, 1)	&&
		tda2014x_w16(c, 0x1B, 7, 1, 0, 1, 6, 1)	&&
		tda2014x_w16(c, 0x18, 0, 1, 0, 1, 6, 0)	&&

		/* ProgramVcoPor */
		tda2014x_r8(c, 0xF, 0, 8, &val)						&&
		(val = (val & 0x1F) | 0x80, tda2014x_w16(c, 0xF, 0, 8, 0, 0, 6, val))		&&
		tda2014x_r8(c, 0x13, 0, 8, &val)						&&
		(val = (val & 0xFFFFFFCF) | 0x20, tda2014x_w16(c, 0x13, 0, 8, 0, 0, 6, val))	&&
		tda2014x_r8(c, 0x12, 0, 8, &val)				&&
		(val |= 0xC0, tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, val))	&&
		tda2014x_w16(c, 0x10, 5, 1, 0, 1, 6, 1)			&&
		tda2014x_w16(c, 0x10, 5, 1, 0, 1, 6, 1)			&&
		tda2014x_w16(c, 0xF, 5, 1, 0, 1, 6, 1)				&&
		tda2014x_r8(c, 0x11, 4, 1, &val)				&&
		(val || tda2014x_r8(c, 0x11, 4, 1, &val))			&&
		(val || tda2014x_r8(c, 0x11, 4, 1, &val))			&&
		val								&&
		tda2014x_r8(c, 0x10, 0, 4, &val)				&&
		tda2014x_w16(c, 0xF, 0, 4, 0, 1, 6, val)			&&
		tda2014x_w16(c, 0xF, 6, 1, 0, 1, 6, 1)				&&
		tda2014x_w16(c, 0xF, 5, 1, 0, 1, 6, 0)				&&
		tda2014x_r8(c, 0x12, 0, 8, &val)				&&
		(val &= 0x7F, tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, val))	&&
		tda2014x_w16(c, 0xD, 5, 2, 0, 1, 6, 1)				&&

		/* EnableLoopThrough */
		tda2014x_r8(c, 6, 0, 8, &val)					&&
		tda2014x_w16(c, 6, 0, 8, 0, 0, 6, (val & 0xF7) | 8)) ? -EIO	:

		tda2014x_tune(fe);
}

static struct i2c_device_id tda2014x_id[] = {
	{TDA2014X_MODNAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, tda2014x_id);

static struct i2c_driver tda2014x_driver = {
	.driver.name	= tda2014x_id->name,
	.probe		= tda2014x_probe,
	.id_table	= tda2014x_id,
};
module_i2c_driver(tda2014x_driver);

MODULE_AUTHOR("Budi Rachmanto, AreMa Inc. <info@are.ma>");
MODULE_DESCRIPTION("Driver for NXP Semiconductors tuner TDA2014x");
MODULE_LICENSE("GPL");