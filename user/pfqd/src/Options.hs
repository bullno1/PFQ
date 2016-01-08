--
--
--  (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
--
--  This program is free software; you can redistribute it and/or modify
--  it under the terms of the GNU General Public License as published by
--  the Free Software Foundation; either version 2 of the License, or
--  (at your option) any later version.
--
--  This program is distributed in the hope that it will be useful,
--  but WITHOUT ANY WARRANTY; without even the implied warranty of
--  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--  GNU General Public License for more details.
--
--  You should have received a copy of the GNU General Public License
--  along with this program; if not, write to the Free Software Foundation,
--  Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
--
--  The full GNU General Public License is included in this distribution in
--  the file called "COPYING".

{-# LANGUAGE DeriveDataTypeable #-}

module Options where

import System.Console.CmdArgs

-- Command line options
--

data Options = Options
    {   config_file    :: String
    ,   dont_rebuild   :: Bool
    }    deriving (Data, Typeable, Show)


options :: Mode (CmdArgs Options)
options = cmdArgsMode $ Options
    {   config_file  = ""    &= typ "FILE" &= help "Config file"
    ,   dont_rebuild = False &= help "Don't rebuild itself"

    } &= summary "pfqd: pfq group manager." &= program "pfqd"

