/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <iomanip>

#include "noisegrain.h"

#include "rtengine/procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

const Glib::ustring NoiseGrain::TOOL_NAME = "noisegrain";

namespace {
constexpr int METHOD_GAUSSIAN = 0;
constexpr int METHOD_POISSON = 1;
constexpr int METHOD_FILM = 2;
}

NoiseGrain::NoiseGrain () : FoldableToolPanel(this, TOOL_NAME, M("TP_NOISEGRAIN_LABEL"), true, true), lastBlurEnabled(false), lastChroma(false)
{

    blurEnabled = Gtk::manage (new Gtk::CheckButton (M("TP_NOISEGRAIN_BLUR")));
    blurEnabled->set_active (false);
    pack_start (*blurEnabled);
    blurEnabledConn = blurEnabled->signal_toggled().connect( sigc::mem_fun(*this, &NoiseGrain::blurEnabledChanged) );

    blurRadius = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_BLURRADIUS"), 0, 100, 0.1, 5));
    pack_start (*blurRadius);
    blurRadius->setAdjusterListener (this);

    pack_start (*Gtk::manage (new Gtk::Separator (Gtk::ORIENTATION_HORIZONTAL)));

    method = Gtk::manage (new MyComboBoxText ());
    method->append (M("TP_NOISEGRAIN_METHOD_GAUSSIAN"));
    method->append (M("TP_NOISEGRAIN_METHOD_POISSON"));
    method->append (M("TP_NOISEGRAIN_METHOD_FILM"));
    method->set_active (METHOD_GAUSSIAN);

    Gtk::Box* const methodHBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_HORIZONTAL));
    methodHBox->pack_start (*Gtk::manage (new Gtk::Label (M("TP_NOISEGRAIN_METHOD") + ":")), Gtk::PACK_SHRINK, 4);
    methodHBox->pack_start (*method);
    pack_start (*methodHBox);

    methodConn = method->signal_changed().connect ( sigc::mem_fun(*this, &NoiseGrain::methodChanged) );

    strength = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_STRENGTH"), 0, 100, 1, 50));
    pack_start (*strength);
    strength->setAdjusterListener (this);

    chroma = Gtk::manage (new Gtk::CheckButton (M("TP_NOISEGRAIN_CHROMA")));
    chroma->set_active (false);
    pack_start (*chroma);
    chromaConn = chroma->signal_toggled().connect( sigc::mem_fun(*this, &NoiseGrain::chromaChanged) );

    filmGrainFrame = Gtk::manage (new Gtk::Frame (M("TP_NOISEGRAIN_FILMFRAME")));
    Gtk::Box* const filmGrainBox = Gtk::manage (new Gtk::Box (Gtk::ORIENTATION_VERTICAL));

    isogr = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_ISOGR"), 20, 6400, 1, 400));
    strengr = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_STRENGR"), 0, 100, 1, 50));
    scalegr = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_SCALEGR"), 0, 100, 1, 100));
    divgr = Gtk::manage (new Adjuster (M("TP_NOISEGRAIN_DIVGR"), 0.2, 3.0, 0.1, 1.5));

    filmGrainBox->pack_start (*isogr);
    filmGrainBox->pack_start (*strengr);
    filmGrainBox->pack_start (*scalegr);
    filmGrainBox->pack_start (*divgr);
    filmGrainFrame->add (*filmGrainBox);
    pack_start (*filmGrainFrame);

    isogr->setAdjusterListener (this);
    strengr->setAdjusterListener (this);
    scalegr->setAdjusterListener (this);
    divgr->setAdjusterListener (this);

    show_all_children ();

    updateGUIState ();
}

void NoiseGrain::updateGUIState ()
{
    const bool film = method->get_active_row_number() == METHOD_FILM;

    strength->set_visible (!film);
    chroma->set_visible (!film);
    filmGrainFrame->set_visible (film);

    blurRadius->set_visible (blurEnabled->get_active());
}

void NoiseGrain::read (const ProcParams* pp, const ParamsEdited* pedited)
{

    disableListener ();

    if (pedited) {
        strength->setEditedState (pedited->grainNoise.strength ? Edited : UnEdited);
        isogr->setEditedState    (pedited->grainNoise.isogr ? Edited : UnEdited);
        strengr->setEditedState  (pedited->grainNoise.strengr ? Edited : UnEdited);
        scalegr->setEditedState  (pedited->grainNoise.scalegr ? Edited : UnEdited);
        divgr->setEditedState    (pedited->grainNoise.divgr ? Edited : UnEdited);
        blurRadius->setEditedState (pedited->grainNoise.blurRadius ? Edited : UnEdited);
        chroma->set_inconsistent (!pedited->grainNoise.chroma);
        blurEnabled->set_inconsistent (!pedited->grainNoise.blurEnabled);
        set_inconsistent         (multiImage && !pedited->grainNoise.enabled);

        if (!pedited->grainNoise.method) {
            method->set_active (3); // "unchanged" row, appended in setBatchMode
        }
    }

    setEnabled (pp->grainNoise.enabled);

    strength->setValue (pp->grainNoise.strength);
    isogr->setValue (pp->grainNoise.isogr);
    strengr->setValue (pp->grainNoise.strengr);
    scalegr->setValue (pp->grainNoise.scalegr);
    divgr->setValue (pp->grainNoise.divgr);
    blurRadius->setValue (pp->grainNoise.blurRadius);

    lastChroma = pp->grainNoise.chroma;
    if (!pedited || pedited->grainNoise.chroma) {
        chroma->set_active (pp->grainNoise.chroma);
    }

    lastBlurEnabled = pp->grainNoise.blurEnabled;
    if (!pedited || pedited->grainNoise.blurEnabled) {
        blurEnabled->set_active (pp->grainNoise.blurEnabled);
    }

    if (!pedited || pedited->grainNoise.method) {
        if (pp->grainNoise.method == "poisson") {
            method->set_active (METHOD_POISSON);
        } else if (pp->grainNoise.method == "film") {
            method->set_active (METHOD_FILM);
        } else {
            method->set_active (METHOD_GAUSSIAN);
        }
    }

    updateGUIState ();

    enableListener ();
}

void NoiseGrain::write (ProcParams* pp, ParamsEdited* pedited)
{

    pp->grainNoise.enabled = getEnabled();
    pp->grainNoise.strength = strength->getValue ();
    pp->grainNoise.isogr = isogr->getIntValue ();
    pp->grainNoise.strengr = strengr->getIntValue ();
    pp->grainNoise.scalegr = scalegr->getIntValue ();
    pp->grainNoise.divgr = divgr->getValue ();
    pp->grainNoise.chroma = chroma->get_active ();
    pp->grainNoise.blurEnabled = blurEnabled->get_active ();
    pp->grainNoise.blurRadius = blurRadius->getValue ();

    if (method->get_active_row_number() == METHOD_POISSON) {
        pp->grainNoise.method = "poisson";
    } else if (method->get_active_row_number() == METHOD_FILM) {
        pp->grainNoise.method = "film";
    } else {
        pp->grainNoise.method = "gaussian";
    }

    if (pedited) {
        pedited->grainNoise.enabled = !get_inconsistent();
        pedited->grainNoise.strength = strength->getEditedState ();
        pedited->grainNoise.isogr = isogr->getEditedState ();
        pedited->grainNoise.strengr = strengr->getEditedState ();
        pedited->grainNoise.scalegr = scalegr->getEditedState ();
        pedited->grainNoise.divgr = divgr->getEditedState ();
        pedited->grainNoise.chroma = !chroma->get_inconsistent();
        pedited->grainNoise.blurEnabled = !blurEnabled->get_inconsistent();
        pedited->grainNoise.blurRadius = blurRadius->getEditedState ();
        pedited->grainNoise.method = method->get_active_row_number() != 3;
    }
}

void NoiseGrain::setDefaults (const ProcParams* defParams, const ParamsEdited* pedited)
{

    strength->setDefault (defParams->grainNoise.strength);
    isogr->setDefault (defParams->grainNoise.isogr);
    strengr->setDefault (defParams->grainNoise.strengr);
    scalegr->setDefault (defParams->grainNoise.scalegr);
    divgr->setDefault (defParams->grainNoise.divgr);
    blurRadius->setDefault (defParams->grainNoise.blurRadius);

    if (pedited) {
        strength->setDefaultEditedState (pedited->grainNoise.strength ? Edited : UnEdited);
        isogr->setDefaultEditedState (pedited->grainNoise.isogr ? Edited : UnEdited);
        strengr->setDefaultEditedState (pedited->grainNoise.strengr ? Edited : UnEdited);
        scalegr->setDefaultEditedState (pedited->grainNoise.scalegr ? Edited : UnEdited);
        divgr->setDefaultEditedState (pedited->grainNoise.divgr ? Edited : UnEdited);
        blurRadius->setDefaultEditedState (pedited->grainNoise.blurRadius ? Edited : UnEdited);
    } else {
        strength->setDefaultEditedState (Irrelevant);
        isogr->setDefaultEditedState (Irrelevant);
        strengr->setDefaultEditedState (Irrelevant);
        scalegr->setDefaultEditedState (Irrelevant);
        divgr->setDefaultEditedState (Irrelevant);
        blurRadius->setDefaultEditedState (Irrelevant);
    }
}

void NoiseGrain::adjusterChanged (Adjuster* a, double newval)
{
    if (listener && getEnabled()) {
        Glib::ustring value = Glib::ustring::format (std::setw(2), std::fixed, std::setprecision(1), newval);

        if (a == strength) {
            listener->panelChanged (EvGrainNoiseStrength, value);
        } else if (a == isogr) {
            listener->panelChanged (EvGrainNoiseIsogr, value);
        } else if (a == strengr) {
            listener->panelChanged (EvGrainNoiseStrengr, value);
        } else if (a == scalegr) {
            listener->panelChanged (EvGrainNoiseScalegr, value);
        } else if (a == divgr) {
            listener->panelChanged (EvGrainNoiseDivgr, value);
        } else if (a == blurRadius) {
            listener->panelChanged (EvGrainNoiseBlurRadius, value);
        }
    }
}

void NoiseGrain::enabledChanged ()
{
    if (listener) {
        if (get_inconsistent()) {
            listener->panelChanged (EvGrainNoiseEnabled, M("GENERAL_UNCHANGED"));
        } else if (getEnabled()) {
            listener->panelChanged (EvGrainNoiseEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvGrainNoiseEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void NoiseGrain::methodChanged ()
{
    updateGUIState ();

    if (listener && getEnabled()) {
        listener->panelChanged (EvGrainNoiseMethod, method->get_active_text());
    }
}

void NoiseGrain::chromaChanged ()
{
    if (batchMode) {
        if (chroma->get_inconsistent()) {
            chroma->set_inconsistent (false);
            chromaConn.block (true);
            chroma->set_active (false);
            chromaConn.block (false);
        } else if (lastChroma) {
            chroma->set_inconsistent (true);
        }

        lastChroma = chroma->get_active ();
    }

    if (listener && getEnabled()) {
        if (chroma->get_active ()) {
            listener->panelChanged (EvGrainNoiseChroma, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvGrainNoiseChroma, M("GENERAL_DISABLED"));
        }
    }
}

void NoiseGrain::blurEnabledChanged ()
{
    if (batchMode) {
        if (blurEnabled->get_inconsistent()) {
            blurEnabled->set_inconsistent (false);
            blurEnabledConn.block (true);
            blurEnabled->set_active (false);
            blurEnabledConn.block (false);
        } else if (lastBlurEnabled) {
            blurEnabled->set_inconsistent (true);
        }

        lastBlurEnabled = blurEnabled->get_active ();
    }

    updateGUIState ();

    if (listener && getEnabled()) {
        if (blurEnabled->get_active ()) {
            listener->panelChanged (EvGrainNoiseBlurEnabled, M("GENERAL_ENABLED"));
        } else {
            listener->panelChanged (EvGrainNoiseBlurEnabled, M("GENERAL_DISABLED"));
        }
    }
}

void NoiseGrain::setBatchMode (bool batchMode)
{

    ToolPanel::setBatchMode (batchMode);
    strength->showEditedCB ();
    isogr->showEditedCB ();
    strengr->showEditedCB ();
    scalegr->showEditedCB ();
    divgr->showEditedCB ();
    blurRadius->showEditedCB ();
    method->append (M("GENERAL_UNCHANGED"));
}
