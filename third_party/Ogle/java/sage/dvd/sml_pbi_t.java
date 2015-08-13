/*
 * Copyright 2015 The SageTV Authors. All Rights Reserved.
 *
 * Adapted from Ogle - A video player
 * Copyright (C) 2000, 2001 Håkan Hjort
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */
package sage.dvd;

public class sml_pbi_t extends Struct
{
  public Unsigned16 category = new Unsigned16();
  public Unsigned32 ilvu_ea = new Unsigned32();
  public Unsigned32 ilvu_sa = new Unsigned32();
  public Unsigned16 size = new Unsigned16();
  public Unsigned32 vob_v_s_ptm = new Unsigned32();
  public Unsigned32 vob_v_e_ptm = new Unsigned32();
  public sml_pbi_audio_t[] vob_a = (sml_pbi_audio_t[]) array(new sml_pbi_audio_t[8]);
}