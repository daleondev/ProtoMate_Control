#!/usr/bin/env python3
"""Component-limit and routed-copper budgets; never substitute for bench results."""
import json
from design import ROOT, REVISION

def threshold(top,bottom,reference):
    return reference*(1+top/bottom)

def main():
    uv=[threshold(143000*.99,10000*1.01,1.175),threshold(143000,10000,1.19),threshold(143000*1.01,10000*.99,1.225)]
    ov=[threshold(226000*.99,10000*1.01,1.17),threshold(226000,10000,1.19),threshold(226000*1.01,10000*.99,1.225)]
    routes=json.loads((ROOT/'manufacturing/verification/power-routing.json').read_text())
    # Qualified-load design envelopes, not every converter's simultaneous rating.
    # DF23 has four 0.3A backlight contacts and one 0.3A TFT contact.
    bkl,logic,giga=1.2,.45,.75
    display_input=(3.3*bkl+5*logic)/(18*.8)
    total_input=display_input+12*giga/(18*.8)
    cable_feed_ohm=.0175*.1/.0105/2*1.3
    # TPS7A91 1% over line/load/temperature; divider initial tolerance,
    # independent 25ppm/K drift over 100K from 25C, and FB leakage.
    nominal=.8*(1+31.2/10)
    rtol=.001+25e-6*100
    fb_error=100e-9*31200*(1+rtol)
    low=.8*.99*(1+31.2*(1-rtol)/(10*(1+rtol)))-fb_error
    high=.8*1.01*(1+31.2*(1+rtol)/(10*(1-rtol)))+fb_error
    # Keep a visible 100mV allowance for dynamic effects, not a claimed guarantee.
    logic_drop=routes['maximum_logic_supply_drop_V']+logic*routes['return_allowance_ohm']+.03
    minimum=low-logic_drop-.1
    # Independent backlight converter; it cannot pull the TFT rail down directly.
    bkl_low=3.3*(1-.02-.005-.01-.0002*45)
    bkl_drop=routes['maximum_backlight_supply_drop_V']*bkl/1.05+bkl*routes['return_allowance_ohm']+.03
    bkl_min=bkl_low-.05-bkl_drop
    # 47nF including +/-20% capacitance envelope; INR/SS = 4..9uA.
    rise_ms=[.8*47e-9*.8/9e-6*1e3,.8*47e-9/6.2e-6*1e3,.8*47e-9*1.2/4e-6*1e3]
    vin_max=5*(1+.02+.005+.01+.0002*45)+.05
    dissipation=(vin_max-low)*logic
    assert ov[2]<36 and total_input < (12000/8060)*.9
    assert display_input/2 < .5 and minimum > 3.0 and high+.025 < 3.6
    assert 1<rise_ms[0]<rise_ms[-1]<20
    assert bkl_min>3.0
    result={
      'revision':REVISION,'status':'CALCULATED DESIGN BUDGET; FIRST-BOARD QUALIFICATION PENDING',
      'input':'24V nominal, regulated +/-10%, >=2A source available; common ground',
      'uv_start_min_typ_max_V':uv,'ov_cutoff_min_typ_max_V':ov,
      'efuse_typical_current_limit_A':12000/8060,
      'allocated_loads':{'backlight_A_max_contact_ceiling':bkl,'logic_A':logic,'GIGA_VIN_A_at_12V':giga},
      'combined_input_A_at_18V_80percent_efficiency':total_input,
      'display_input_A_at_18V_80percent_efficiency':display_input,
      'FFC_power_current_per_contact_A':display_input/2,
      'FFC_24V_drop_allowance_V':display_input*(cable_feed_ohm+.1),
      'precision_logic_rail':{'regulator':'TPS7A9101DSKR fed by TSR 1-2450',
        'nominal_V':nominal,'static_min_max_V':[low,high],'routed_supply_return_contact_drop_V':logic_drop,
        'dynamic_drop_allowance_V':.1,'panel_min_with_allowance_V':minimum,
        'panel_requirement_V':[3.0,3.6],'rise_estimate_min_typ_max_ms':rise_ms,
        'soft_start_C_nF':47,'soft_start_current_uA':[4,6.2,9],
        'feedback_resistors_ohm':[31200,10000],'feedback_resistor_tolerance_percent':.1,
        'feedback_resistor_TCR_ppm_per_K':25,'resistor_temperature_excursion_from_25C_K':100,
        'feedback_pin_leakage_limit_nA':100,
        'accuracy_load_range_A':[.005,1.0],
        'output_capacitance_regulator_local_nominal_uF':94,  # C13 + C14; DC-bias derating still applies.
        'additional_external_capacitance':'Display connector and supervisor bypass capacitors, plus GIGA interface decoupling; panel internal capacitance is unspecified.',
        'regulator_minimum_effective_output_capacitance_uF':10,
        'thermal_power_upper_budget_W':dissipation,
        'thermal_JA_datasheet_C_per_W':62.5,
        'junction_estimate_at_55C_ambient_C':55+dissipation*62.5,
        'thermal_qualification':'Solder exposed pad; ground copper and external thermal stitching. Qualify <=55C enclosure ambient at 0.45A. JEDEC thetaJA is an estimate, not a measured board thermal result.'},
      'backlight_rail':{'regulator':'TSR 2-2433; independent from precision logic supply',
        'nominal_V':3.3,'static_min_before_ripple_and_copper_V':bkl_low,
        'ripple_negative_allowance_V':.05,'routed_return_contact_drop_V':bkl_drop,
        'datasheet_ripple_Vpp_typical':.05,
        'ripple_limit_status':'The retained TSR2 datasheet gives a typical value, not a guaranteed maximum. The negative ripple allowance is a design assumption to qualify.',
        'static_min_with_allowances_V':bkl_min,'nominal_external_capacitance_uF':188,
        'converter_capacitive_load_limit_uF':900,'unknown_panel_capacitance':'Included in the 900uF limit; first-board check.',
        'maximum_datasheet_50percent_load_step_excursion_V':.25,
        'qualification':'Measure PWM/load-step behaviour. The converter transient bound can cross the 3.07V supervisor threshold; verify no retriggering at requested brightness. Hardware inhibits the panel rather than relying on operation during undervoltage.',
        'connector_contacts':[5,6,7,8],'contact_rating_A':.3,
        'supply_voltage_decision':'3.3V retained: the panel connector table does not authorize 5V at these contacts.'},
      'sequencing':{'supervisor':'Two TPS3808G33DBVR, one per rail, open-drain outputs wired AND',
        'falling_threshold_nominal_V':3.07,'release_delay_nominal_ms':300,
        'delay_configuration':'100k from CT to +3V3_PANEL; VDD and MR tied to +3V3_PANEL, ensuring supervision remains valid whenever the gates are powered',
        'gating':'SN74LVC08A AND gates PWM, STANDBY, TFT_RESET_N and TOUCH_RESET_N with READY; 10k output pull-downs.',
        'host_ready':'FFC GIGA.10 -> display.71; AXC1 isolation -> PI1 / D70, pull-down when absent.',
        'firmware':'Rendering continues without panel power. Return of READY repeats reset/standby delays; backlight waits 200ms after standby release and then for a valid scan boundary.'},
      'limitations':['No physical panel is connected. Static arithmetic and component specifications do not establish transient or thermal qualification.',
        'Backlight maximum current and contact sharing are unspecified by Smartwin; measure full brightness and remain within every 0.3A contact rating.',
        'Both rail ramps, brownout/recovery, temperature, ripple and full-brightness current are first-board acceptance measurements.',
        'No certified surge/EMC claim is made.']}
    (ROOT/'manufacturing/verification/power-budget.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
